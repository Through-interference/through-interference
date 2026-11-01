#include "tx/TxController.h"

#include <RadioLib.h>
#include "crypto.h"
#include "protocol.h"
#include "radio_config.h"
#include "radio_hardware.h"
#include "RadioChannelManager.h"

namespace {
const unsigned long ACK_TIMEOUT_MS = 3000;
const int MAX_RETRIES = 3;
const int DEGRADED_AFTER_ACK_TIMEOUTS = 2;

const unsigned long NEXT_DATA_DELAY_MS = 1000UL;
const unsigned long DATA_TX_TIMEOUT_MS = 5000UL;
}

void TxController::begin() {
    Serial.begin(115200);
    delay(1000);

    if (!cryptoInit()) {
        Serial.println("AES-GCM initialization failed!");
        while (true) delay(1000);
    }

    if (!outbox_.begin()) {
        Serial.println("[TX] Cannot open NVS namespace for message IDs");
        while (true) delay(1000);
    }

    Serial.print("[TX] Node ID: ");
    Serial.println(TX_NODE_ID);

    Serial.print("[TX] Default destination ID: ");
    Serial.println(RX_NODE_ID);

    radio.setRfSwitchPins(LORA_RXEN, LORA_TXEN);
    radio.tcxoVoltage = 1.8;

    Serial.print("[SX1262] Initializing ... ");

    ConfigLoRa_t config;
    config.frequency = radioChannelManager.currentFrequencyMHz();

    const int state = radio.begin(config);

    if (state != RADIOLIB_ERR_NONE) {
        Serial.print("Radio initialization failed: ");
        Serial.println(state);
        while (true) delay(1000);
    }

    Serial.println("success!");

    radio.setDio1Action(setFlag);

    Serial.println("Encrypted transmitter started");

    if (!outbox_.restore()) {
        if (outbox_.sequence() <= TOTAL_MESSAGES) {
            startNextData();
        } else {
            Serial.println(
                "[TX] No pending outbox and test sequence already complete"
            );
        }
    } else {
        // Restore the same logical message, encrypted with a fresh crypto counter.
        retryCount_ = 0;
        nextDataPending_ = false;
        sendCurrentData();
    }

    // All earlier sequence numbers were ACKed before next_seq advanced.
    ackCount_ = (outbox_.sequence() > TOTAL_MESSAGES)
        ? TOTAL_MESSAGES
        : static_cast<int>(outbox_.sequence() - 1);
}


void TxController::update() {
    if (outbox_.sequence() > TOTAL_MESSAGES) {
        printSummary();
        while (true) delay(1000);
    }

    handleRadioEvent();
    handleTxWatchdog();

    if (radioChannelManager.isSearching()) {
        updateChannelSearch();
    } else {
        handleAckTimeout();
    }

    handleScheduledData();

    yield();
}


void TxController::startNextData() {
    if (!outbox_.isActive() && !outbox_.create()) {
        if (outbox_.sequence() > TOTAL_MESSAGES) return;

        Serial.println(
            "[TX][FATAL] Could not create a persistent outbox record"
        );

        while (true) delay(1000);
    }

    retryCount_ = 0;
    nextDataPending_ = false;
    consecutiveAckTimeouts_ = 0;

    sendCurrentData();
}


void TxController::sendCurrentData() {
    radio.standby();
    operationDone = false;

    Serial.print("[TX] Sending DATA messageId=");
    Serial.println(outbox_.messageId());

    // Always retransmit the already encrypted Outbox frame.
    String frame = outbox_.encryptedFrame();

    transmissionState_ = radio.startTransmit(frame);

    if (transmissionState_ == RADIOLIB_ERR_NONE) {
        transmitFlag_ = true;
        dataTxStartedAt_ = millis();
        ackTimer_ = millis();
    } else {
        Serial.print("[TX] startTransmit failed: ");
        Serial.println(transmissionState_);

        transmitFlag_ = false;

        // Treat a start failure as an expired ACK timeout.
        ackTimer_ = millis() - ACK_TIMEOUT_MS - 1;
    }
}


void TxController::handleRadioEvent() {
    if (!operationDone) return;

    operationDone = false;

    if (transmitFlag_) {
        transmissionState_ = radio.finishTransmit();

        if (transmissionState_ == RADIOLIB_ERR_NONE) {
            Serial.println("[TX] Transmission finished");
        } else {
            Serial.print("[TX] Transmission error: ");
            Serial.println(transmissionState_);
        }

        transmitFlag_ = false;

        const int state = radio.startReceive();

        if (state != RADIOLIB_ERR_NONE) {
            Serial.print("[TX] startReceive failed: ");
            Serial.println(state);

            if (radioChannelManager.isSearching()) {
                nextSearchTxAt_ = millis();
            } else {
                ackTimer_ = millis() - ACK_TIMEOUT_MS - 1;
            }
        } else {
            // Start ACK timeout only after DATA transmission has completed.
            ackTimer_ = millis();
        }

        return;
    }

    handleReceivedAck();
}


void TxController::handleReceivedAck() {
    String encryptedFrame;

    const int state = radio.readData(encryptedFrame);

    if (state != RADIOLIB_ERR_NONE) {
        Serial.print("[TX] readData failed: ");
        Serial.println(state);

        radio.startReceive();
        return;
    }

    Serial.print("[TX] Received encrypted frame: ");
    Serial.println(encryptedFrame);

    Serial.print("[TX] RSSI: ");
    Serial.print(radio.getRSSI());
    Serial.println(" dBm");

    Serial.print("[TX] SNR: ");
    Serial.print(radio.getSNR());
    Serial.println(" dB");

    String plaintext;

    uint32_t senderId = 0;
    uint64_t cryptoCounter = 0;

    const CryptoPacketStatus cryptoStatus = decryptPacketEx(
        encryptedFrame,
        plaintext,
        senderId,
        cryptoCounter
    );

    if (cryptoStatus != CRYPTO_PACKET_NEW &&
        cryptoStatus != CRYPTO_PACKET_DUPLICATE) {

        Serial.println(
            "[TX] ACK failed authentication/replay check; ignored"
        );

        radio.startReceive();
        return;
    }

    RadioMessage received;

    const bool matchingAck =
        decodeRadioMessage(plaintext, received) &&
        received.type == RADIO_MSG_ACK &&
        received.senderId == RX_NODE_ID &&
        received.destinationId == TX_NODE_ID &&
        received.messageId == outbox_.messageId() &&
        received.payload == "OK" &&
        senderId == static_cast<uint32_t>(CRYPTO_PEER_ID);

    if (!matchingAck) {
        Serial.print(
            "[TX] ACK does not match current transaction; ignored. "
            "Current messageId="
        );

        Serial.println(outbox_.messageId());

        radio.startReceive();
        return;
    }

    Serial.print("[TX] ACK matched messageId=");
    Serial.println(received.messageId);

    // Commit the ACK to persistent Outbox.
    if (!outbox_.acknowledge()) {
        Serial.println(
            "[TX] ACK was valid, but outbox commit failed; "
            "transaction remains pending"
        );

        ackTimer_ = millis() - ACK_TIMEOUT_MS - 1;

        radio.startReceive();
        return;
    }

    // Determine whether the ACK was received during channel search.
    const bool recoveredBySearch =
        radioChannelManager.isSearching();

    if (recoveredBySearch) {
        radioChannelManager.stopSearch();

        switchAfterAck_ = false;

        Serial.print("[TX][CHANNEL] Connection restored on channel ");
        Serial.println(radioChannelManager.currentChannel());

    } else if (switchAfterAck_) {

        // Soft failover: switch before sending the next DATA.
        if (radioChannelManager.switchToNext()) {
            Serial.print(
                "[TX][CHANNEL] Switching for next DATA to channel "
            );

            Serial.println(radioChannelManager.currentChannel());

            switchAfterAck_ = false;

        } else {
            // Keep the flag: try switching again after the next ACK.
            Serial.println(
                "[TX][CHANNEL] Switch failed; will retry after next ACK"
            );
        }
    }

    ackCount_++;

    retryCount_ = 0;
    consecutiveAckTimeouts_ = 0;

    setConnectionState(CONNECTED);

    // Schedule the next logical message.
    if (outbox_.sequence() <= TOTAL_MESSAGES) {
        nextDataAt_ = millis() + NEXT_DATA_DELAY_MS;
        nextDataPending_ = true;
    }
}


void TxController::handleTxWatchdog() {
    // If TX completion IRQ never arrives, keep the Outbox and retry DATA.
    if (!transmitFlag_ ||
        (unsigned long)(millis() - dataTxStartedAt_) < DATA_TX_TIMEOUT_MS) {
        return;
    }

    Serial.println(
        "[TX] DATA TX completion timeout; resetting radio state, "
        "outbox retained"
    );

    radio.standby();

    if (radioChannelManager.isSearching()) {
        nextSearchTxAt_ = millis();
    } else {
        ackTimer_ = millis() - ACK_TIMEOUT_MS - 1;
    }

    transmitFlag_ = false;
    operationDone = false;
}


void TxController::handleAckTimeout() {
    if (radioChannelManager.isSearching() ||
        transmitFlag_ ||
        nextDataPending_ ||
        outbox_.sequence() > TOTAL_MESSAGES ||
        millis() - ackTimer_ <= ACK_TIMEOUT_MS) {
        return;
    }

    consecutiveAckTimeouts_++;

    Serial.print("[TX] ACK timeout; consecutive timeouts: ");
    Serial.println(consecutiveAckTimeouts_);

    // Mark connection as degraded after consecutive ACK timeouts.
    if (consecutiveAckTimeouts_ >= DEGRADED_AFTER_ACK_TIMEOUTS) {
        if (connectionState_ == CONNECTED) {
            setConnectionState(DEGRADED);
        }

        switchAfterAck_ = true;
    }

    // Perform normal retransmissions first.
    if (retryCount_ < MAX_RETRIES) {
        retryCount_++;
        totalNormalRetries_++;

        Serial.print("[TX] Normal retry ");
        Serial.print(retryCount_);
        Serial.print("/");
        Serial.println(MAX_RETRIES);

        sendCurrentData();
        return;
    }

    // Normal retries exhausted: begin channel search.
    startChannelSearch();
}


void TxController::handleScheduledData() {
    if (!nextDataPending_ ||
        (long)(millis() - nextDataAt_) < 0 ||
        outbox_.sequence() > TOTAL_MESSAGES ||
        transmitFlag_) {
        return;
    }

    nextDataPending_ = false;

    startNextData();
}


void TxController::setConnectionState(ConnectionState nextState) {
    if (connectionState_ == nextState) return;

    const unsigned long now = millis();

    if (connectionState_ == RECOVERY) {
        lastRecoveryDurationMs_ = now - recoveryStartedAt_;
        totalRecoveryTimeMs_ += lastRecoveryDurationMs_;

        if (nextState == CONNECTED) {
            successfulRecoveries_++;

            Serial.print("[TX] Recovery completed in ");
            Serial.print(lastRecoveryDurationMs_);
            Serial.println(" ms");
        }
    }

    connectionState_ = nextState;

    Serial.print("[TX] Connection state: ");

    if (nextState == CONNECTED) {
        Serial.println("CONNECTED");

    } else if (nextState == DEGRADED) {
        Serial.println("DEGRADED");

    } else {
        recoveryStartedAt_ = now;
        recoveryEpisodes_++;

        Serial.println("RECOVERY");
    }
}


void TxController::printSummary() {
    Serial.println("\n=== TEST SUMMARY ===");

    Serial.print("Total messages: ");
    Serial.println(TOTAL_MESSAGES);

    Serial.print("ACK received: ");
    Serial.println(ackCount_);

    Serial.println(
        "Messages permanently discarded: 0 (persistent outbox policy)"
    );

    Serial.print("Retry cycles exhausted while transaction remained pending: ");
    Serial.println(retryCyclesExhausted_);

    Serial.print("ACK rate: ");
    Serial.print(100.0 * ackCount_ / TOTAL_MESSAGES);
    Serial.println("%");

    Serial.print("Normal retries: ");
    Serial.println(totalNormalRetries_);

    Serial.print("Recovery attempts: ");
    Serial.println(totalRecoveryAttempts_);

    Serial.print("Recovery episodes: ");
    Serial.println(recoveryEpisodes_);

    Serial.print("Successful recoveries: ");
    Serial.println(successfulRecoveries_);

    Serial.print("Accumulated recovery time: ");
    Serial.print(totalRecoveryTimeMs_);
    Serial.println(" ms");

    if (connectionState_ == RECOVERY) {
        Serial.print("Unfinished recovery duration: ");
        Serial.print(millis() - recoveryStartedAt_);
        Serial.println(" ms");
    }

    Serial.println("Test complete.");
}


void TxController::startChannelSearch() {
    if (radioChannelManager.isSearching()) return;

    Serial.println("[TX][CHANNEL] Starting channel search");

    // Switch frequency first. Enter RECOVERY only if switching succeeds.
    if (!radioChannelManager.startSearch(TX_CHANNEL_DWELL_MS)) {
        Serial.println("[TX][CHANNEL] Cannot start channel search");

        // Avoid retrying startSearch on every update() iteration.
        ackTimer_ = millis();

        return;
    }

    setConnectionState(RECOVERY);

    totalRecoveryAttempts_++;

    nextSearchTxAt_ = millis();
}


void TxController::updateChannelSearch() {
    if (!radioChannelManager.isSearching() || transmitFlag_) {
        return;
    }

    const uint8_t previousChannel =
        radioChannelManager.currentChannel();

    if (radioChannelManager.updateSearch()) {
        totalRecoveryAttempts_++;

        if (radioChannelManager.currentChannel() == RADIO_PRIMARY_CHANNEL &&
            previousChannel != RADIO_PRIMARY_CHANNEL) {

            retryCyclesExhausted_++;
        }

        Serial.print("[TX][CHANNEL] Searching on channel ");
        Serial.println(radioChannelManager.currentChannel());
    }

    // Repeat the same persistent DATA frame while searching.
    if ((long)(millis() - nextSearchTxAt_) >= 0) {
        nextSearchTxAt_ = millis() + TX_SEARCH_REPEAT_MS;

        Serial.println("[TX][CHANNEL] Repeating pending DATA");

        sendCurrentData();
    }
}
