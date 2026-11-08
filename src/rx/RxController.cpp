#include "rx/RxController.h"

#include <RadioLib.h>
#include "crypto.h"
#include "protocol.h"
#include "radio_config.h"
#include "radio_hardware.h"
#include "RadioChannelManager.h"
#include "AtCommandManager.h"

namespace {
const uint8_t TEST_DROP_ACK_COUNT = 0;

const unsigned long ACK_TX_TIMEOUT_MS = 5000UL;
const unsigned long RX_STATS_INTERVAL_MS = 50000UL;
const int FC_UART_RX = 16;
const int FC_UART_TX = 17;

static ModemConfig activeRadioConfig;
static bool hasActiveConfig = false;

bool applyModemConfigToRadio(const ModemConfig& newCfg) {
    radio.standby();

    if (!radioChannelManager.setPrimaryFrequency(newCfg.primFreqMhz)) {
        Serial.println("[RX][AT] Failed to set primary frequency");
        return false;
    }

    if (!radioChannelManager.setBackupFrequency(newCfg.backFreqMhz)) {
        Serial.println("[RX][AT] Failed to set backup frequency");
        return false;
    }
    const float currentFreq = radioChannelManager.currentFrequencyMHz();
    int16_t state = RADIOLIB_ERR_NONE;

    switch (newCfg.modulation) {
        case classModulation::GFSK100:
            state = radio.beginFSK(
                currentFreq, 100.0f, 50.0f, 234.3f,
                newCfg.txPowerDbm, 16, 1.8f
            );
            break;

        case classModulation::GFSK250:
            state = radio.beginFSK(
                currentFreq, 250.0f, 125.0f, 467.0f,
                newCfg.txPowerDbm, 16, 1.8f
            );
            break;

        case classModulation::LORA_SF7: {
            ConfigLoRa_t loraCfg;
            loraCfg.frequency = currentFreq;

            state = radio.begin(loraCfg);

            if (state == RADIOLIB_ERR_NONE) {
                state = radio.setSpreadingFactor(7);
            }
            break;
        }

        case classModulation::LORA_SF9: {
            ConfigLoRa_t loraCfg;
            loraCfg.frequency = currentFreq;

            state = radio.begin(loraCfg);

            if (state == RADIOLIB_ERR_NONE) {
                state = radio.setSpreadingFactor(9);
            }
            break;
        }

        default:
            Serial.println("[RX][AT] Unsupported modulation");
            return false;

    }

    if (state == RADIOLIB_ERR_NONE) {
        state = radio.setOutputPower(newCfg.txPowerDbm);
    }

    radio.setRfSwitchPins(LORA_RXEN, LORA_TXEN);
    radio.setDio1Action(setFlag);

    if (state != RADIOLIB_ERR_NONE) {
        Serial.print("[RX][AT] Radio reconfiguration error: ");
        Serial.println(state);
        return false;
    }

    bool hasCustomKey = false;

    for (int i = 0; i < 16; i++) {
        if (newCfg.aesKey[i] != 0) {
            hasCustomKey = true;
            break;
        }
    }

    if (hasCustomKey) {
        if (cryptoUpdateKey(newCfg.aesKey)) {
            Serial.println("[RX][AT] AES-128 key updated");
        } else {
            Serial.println("[RX][AT] AES-128 key update FAILED");
            return false;
        }
    }

    activeRadioConfig = newCfg;
    hasActiveConfig = true;

    return true;
}

bool reinitRadioFromRecovery() {
    if (!hasActiveConfig) return false;
    return applyModemConfigToRadio(activeRadioConfig);
}
}
AtCommandManager rxAtManager(Serial, 255);

RxController::RxController()
: radioRecovery_(transmitFlag_, operationDone) {
}


void RxController::begin() {
    Serial.begin(115200);
    delay(1000);

    // Загружаем сопряженный конфиг из NVS
    rxAtManager.begin();

    Serial2.setRxBufferSize(1024);
    Serial2.begin(57600, SERIAL_8N1, FC_UART_RX, FC_UART_TX);
   

    if (!cryptoInit()) {
        Serial.println("AES-GCM initialization failed!");
        while (true) delay(1000);
    }

    if (!messageStore_.begin()) {
        Serial.println(
            "[RX] Cannot open NVS namespace for transaction state"
        );
        while (true) delay(1000);
    }

    if (!messageStore_.load()) {
        Serial.println(
            "[RX][FATAL] Could not load persistent transaction state; "
            "stopping for safety"
        );
        while (true) delay(1000);
    }

    Serial.print("[RX] Node ID: ");
    Serial.println(RX_NODE_ID);

    Serial.print("[RX] Expected source node ID: ");
    Serial.println(TX_NODE_ID);

    Serial.print("[RX] Last committed messageId: ");
    Serial.println(messageStore_.lastMessageId());

    radioRecovery_.setReinitCallback(reinitRadioFromRecovery);

    Serial.print("[SX1262] Initializing ... ");

    if (radioRecovery_.initializeRadio()) {
        Serial.println("success!");

        // Применяем сохраненные через bind.py настройки из NVS сразу при старте
        ModemConfig bootCfg;

        bool hasValidKey = false;
        bool configApplied = false;

        if (rxAtManager.takePendingConfig(bootCfg)) {
            configApplied = applyModemConfigToRadio(bootCfg);

            if (configApplied) {
                Serial.println(
                    "[RX][BOOT] Loaded and applied bind config from NVS"
                );

                for (int i = 0; i < 16; i++) {
                    if (bootCfg.aesKey[i] != 0) {
                        hasValidKey = true;
                        break;
                    }
                }
            } else {
                Serial.println(
                    "[RX][BOOT] Failed to apply bind config from NVS"
                );
            }
        }

        Serial.println("Encrypted receiver started");

        // Разрешаем радиообмен только при успешном применении конфига
        // и наличии ненулевого AES-ключа.
        if (!configApplied || !hasValidKey) {
            Serial.println(
                "[CRITICAL] Radio transmission BLOCKED. "
                "Run bind.py to configure encryption."
            );

            while (true) {
                rxAtManager.processInput();
                delay(50);
            }
        }

        radioRecovery_.startReceiveWithRecovery();
    } else {
        Serial.println(
            "initialization failed; recovery will retry automatically"
        );

        radioRecovery_.scheduleFullRecovery(
            "startup initialization failed"
        );
    }

    lastStatsAt_ = millis();
    lastDataAt_ = millis();

    duplicateStreak_ = 0;
    scanAfterAck_ = false;
}


void RxController::update() {
    // 1. Обработка сервисных команд на приемнике (включая bind.py)
    rxAtManager.processInput();

    // 2. Применяем конфигурацию только когда радио свободно:
    // не передаёт, не ожидает обработки прерывания и не восстанавливается.
    if (!transmitFlag_ &&
        !operationDone &&
        !radioRecovery_.isRecovering()) {

        ModemConfig newCfg;

        if (rxAtManager.takePendingConfig(newCfg)) {
            if (applyModemConfigToRadio(newCfg)) {
                if (radioRecovery_.startReceiveWithRecovery()) {
                    Serial.println(
                        "[RX][AT] Applied new radio config from AT commands"
                    );
                } else {
                    Serial.println(
                        "[RX][AT] Config applied, but RX restart is pending"
                    );
                }
            } else {
                Serial.println(
                    "[RX][AT] Failed to apply radio config"
                );

                rxAtManager.retryPendingConfig();

                radioRecovery_.scheduleFullRecovery(
                    "AT radio configuration failed"
                );
            }
        }
    }

    // 3. Обновляем телеметрию для мониторинга
    ModemTelemetry telem;
    telem.rssiDbm = radio.getRSSI();
    telem.snrDb = radio.getSNR();
    telem.txPackets = ackTransmissionsCompleted_;
    telem.rxPackets = receivedDataFrames_;
    telem.linkState = radioChannelManager.isSearching() ? classLinkState::LOST : classLinkState::TRACKING;
    rxAtManager.updateTelemetry(telem);

    handleRadioEvent();

    if (transmitFlag_ &&
        (unsigned long)(millis() - ackTxStartedAt_) >=
        ACK_TX_TIMEOUT_MS) {

        Serial.println(
            "[RX] ACK TX timeout; scheduling radio reinitialization"
        );

        radio.standby();

        transmitFlag_ = false;
        operationDone = false;

        radioRecovery_.scheduleFullRecovery(
            "ACK transmission timeout"
        );
    }

    radioRecovery_.update();
    updateChannelSearch();
    printPeriodicStats();

    yield();
}


void RxController::handleRadioEvent() {
if (!operationDone) return;

operationDone = false;

// ACK transmission has completed: return to receive mode.
if (transmitFlag_) {
     transmissionState_ = radio.finishTransmit();

     if (transmissionState_ == RADIOLIB_ERR_NONE) {
         ackTransmissionsCompleted_++;
         Serial.println("[RX] ACK transmission finished");
     } else {
         ackTransmissionFailures_++;
         Serial.print("[RX] ACK transmission error: ");
         Serial.println(transmissionState_);
     }

     transmitFlag_ = false;

     // ДОБАВИТЬ: Переключаем модуляцию сразу после того, как ACK улетел в эфир!
     if (pendingModSwitchAfterAck_) {
         pendingModSwitchAfterAck_ = false;

         ModemConfig cfg = rxAtManager.getCurrentConfig();
         if (pendingModName_ == "GFSK100") cfg.modulation = classModulation::GFSK100;
         else if (pendingModName_ == "GFSK250") cfg.modulation = classModulation::GFSK250;
         else if (pendingModName_ == "LORA_SF7") cfg.modulation = classModulation::LORA_SF7;
         else if (pendingModName_ == "LORA_SF9") cfg.modulation = classModulation::LORA_SF9;

         if (applyModemConfigToRadio(cfg)) {
             Serial.print("[RX][OTA] Switched modulation to ");
             Serial.println(pendingModName_);
             // Сохраняем в NVS приемника через Preferences напрямую:
             Preferences p;
             if (p.begin("modem_cfg", false)) {
                 p.putBytes("cfg_blob", &cfg, sizeof(ModemConfig));
                 p.end();
                 Serial.println("[RX][OTA] Saved new modulation to NVS");
             }
         }
     }

     if (radioRecovery_.startReceiveWithRecovery()) {
         Serial.println("[RX][STATE] Receive mode restored after ACK");
     }
     return;
 }

handleReceivedData();
}


void RxController::handleReceivedData() {
String encryptedFrame;

const int state = radio.readData(encryptedFrame);

if (state != RADIOLIB_ERR_NONE) {
Serial.print("[RX] readData failed: ");
Serial.println(state);

radioRecovery_.startReceiveWithRecovery();
return;
}

String plaintext;

uint32_t cryptoSenderId = 0;
uint64_t cryptoCounter = 0;

const CryptoPacketStatus status = decryptPacketEx(
    encryptedFrame,
    plaintext,
    cryptoSenderId,
    cryptoCounter,
    false
);

if (status == CRYPTO_PACKET_ERROR) {
decryptFailures_++;

Serial.println(
"[RX] Decryption/authentication FAILED; ignored"
);

radioRecovery_.startReceiveWithRecovery();

} else if (status == CRYPTO_PACKET_REPLAY) {
decryptFailures_++;

Serial.println(
"[RX] Old crypto frame/replay; ignored"
);

radioRecovery_.startReceiveWithRecovery();

} else {
processApplicationMessage(
    status,
    plaintext,
    cryptoSenderId,
    cryptoCounter
);
}
}


void RxController::processApplicationMessage(
CryptoPacketStatus cryptoStatus,
const String& plaintext,
uint32_t cryptoSenderId,
uint64_t cryptoCounter
) {
    if (cryptoStatus != CRYPTO_PACKET_NEW &&
    cryptoStatus != CRYPTO_PACKET_DUPLICATE) {

    decryptFailures_++;

    Serial.println(
    "[RX] Invalid crypto status; message ignored"
    );

    radioRecovery_.startReceiveWithRecovery();
    return;
    }

    // Explicit cryptographic peer check.
    if (cryptoSenderId != static_cast<uint32_t>(CRYPTO_PEER_ID)) {
    Serial.println(
    "[RX] Unexpected cryptographic peer; ignored"
    );

    radioRecovery_.startReceiveWithRecovery();
    return;
    }

    RadioMessage message;

    if (!decodeRadioMessage(plaintext, message)) {
        Serial.println("[RX] Invalid application header; ignored");
        radioRecovery_.startReceiveWithRecovery();
        return;
    }

    // HEARTBEAT: обновляем активность, но не выполняем команду.
    if (message.type == RADIO_MSG_HEARTBEAT) {

        if (cryptoStatus != CRYPTO_PACKET_NEW ||
            message.senderId != TX_NODE_ID ||
            message.destinationId != RX_NODE_ID ||
            message.messageId != 0 ||
            message.payload != "PING") {

            cryptoCommitRxCounter(cryptoCounter);
            radioRecovery_.startReceiveWithRecovery();
            return;
        }

        lastDataAt_ = millis();

        if (radioChannelManager.isSearching()) {
            radioChannelManager.stopSearch();
        }

        duplicateStreak_ = 0;
        scanAfterAck_ = false;

        radioRecovery_.startReceiveWithRecovery();
        return;
    }

    // Check application message type and addressing.
    if (message.type != RADIO_MSG_DATA ||
    message.senderId != TX_NODE_ID ||
    message.destinationId != RX_NODE_ID) {

    Serial.print("[RX] Wrong type/address. sender=");
    Serial.print(message.senderId);

    Serial.print(" destination=");
    Serial.println(message.destinationId);

    radioRecovery_.startReceiveWithRecovery();
    return;
    }
    // Показываем радиопараметры только для команд дрона.
    Serial.print("[RX] RSSI: ");
    Serial.print(radio.getRSSI());
    Serial.println(" dBm");

    Serial.print("[RX] SNR: ");
    Serial.print(radio.getSNR());
    Serial.println(" dB");

    // A crypto duplicate is valid only for the already committed
    // logical message with exactly the same contents.
    if (cryptoStatus == CRYPTO_PACKET_DUPLICATE &&
    messageStore_.hasMessage() &&
    message.messageId == messageStore_.lastMessageId() &&
    (message.senderId != messageStore_.lastSourceId() ||
    message.destinationId != messageStore_.lastDestinationId() ||
    message.payload != messageStore_.lastPayload())) {

    Serial.println(
    "[RX] Crypto duplicate does not match the committed "
    "logical transaction; rejected"
    );

    radioRecovery_.startReceiveWithRecovery();
    return;
    }

    // We received an authenticated DATA from the expected TX.
    lastDataAt_ = millis();

    // If RX was searching, the transmitting channel has been found.
    if (radioChannelManager.isSearching()) {
    radioChannelManager.stopSearch();

    duplicateStreak_ = 0;
    scanAfterAck_ = false;

    Serial.print("[RX][CHANNEL] Found TX on channel ");
    Serial.println(radioChannelManager.currentChannel());
    }

    receivedDataFrames_++;

    Serial.print("[RX] DATA messageId=");
    Serial.print(message.messageId);

    Serial.print(" from node ");
    Serial.print(message.senderId);

    Serial.print(" to node ");
    Serial.println(message.destinationId);

    // Reject stale logical transactions.
    if (messageStore_.hasMessage() &&
    message.messageId < messageStore_.lastMessageId()) {

    Serial.println(
    "[RX] Stale logical transaction; not executed or ACKed"
    );

    radioRecovery_.startReceiveWithRecovery();
    return;
    }

    // Handle a repeated logical transaction.
    if (messageStore_.hasMessage() &&
    message.messageId == messageStore_.lastMessageId()) {

    if (message.senderId != messageStore_.lastSourceId() ||
    message.destinationId != messageStore_.lastDestinationId() ||
    message.payload != messageStore_.lastPayload()) {

    Serial.println(
    "[RX] Message ID reused with different content; rejected"
    );

    radioRecovery_.startReceiveWithRecovery();
    return;
    }

    duplicateDataFrames_++;

    if (duplicateStreak_ < 255) {
    duplicateStreak_++;
    }

    Serial.print("[RX] Consecutive duplicate DATA frames: ");
    Serial.println(duplicateStreak_);

    // Multiple retransmissions indicate a possible connection problem.
    if (duplicateStreak_ >= RX_DUPLICATES_BEFORE_SCAN) {
    scanAfterAck_ = true;
    }

    Serial.println(
    "[RX] Duplicate logical transaction; "
    "action NOT repeated, ACK resent"
    );

    if (!cryptoCommitRxCounter(cryptoCounter)) {
        radioRecovery_.startReceiveWithRecovery();
        return;
    }

    startAck(message.messageId);
    return;
    }

    // A new logical message was received.
    // Persist it before acknowledging it.
    // Сначала фиксируем криптографический счётчик.
    if (!cryptoCommitRxCounter(cryptoCounter)) {
        Serial.println("[RX] Crypto counter commit failed");
        radioRecovery_.startReceiveWithRecovery();
        return;
    }

    // Затем сохраняем идентификатор команды.
    if (!messageStore_.save(message)) {
        Serial.println(
            "[RX] Cannot persist transaction state; "
            "action and ACK suppressed"
        );

        radioRecovery_.startReceiveWithRecovery();
        return;
    }

    // Сохраняем криптографический счётчик после сообщения.
    if (!cryptoCommitRxCounter(cryptoCounter)) {
        Serial.println("[RX] Crypto counter commit failed");
    }

    // New DATA resets the duplicate detection streak.
    duplicateStreak_ = 0;
    scanAfterAck_ = false;

    // Commit messageId before the application action (at-most-once gate).
    newDataMessages_++;

    Serial.print("[RX] New DATA accepted once. Payload: ");
    Serial.println(message.payload);

    if (message.payload.startsWith("SYS:MOD:")) {
        pendingModName_ = message.payload.substring(8);
        pendingModName_.trim();
        pendingModSwitchAfterAck_ = true;
        Serial.print("[RX][OTA] Will switch modulation after ACK to: ");
        Serial.println(pendingModName_);
    } else {
        // Обычные боевые команды пересылаем в полетный контроллер:
        Serial2.println(message.payload);
    }

    startAck(message.messageId);
}


void RxController::startAck(uint32_t messageId) {
// Test mode: deliberately drop the first ACK.
// Simulate loss of the first N ACK transmissions.
if (testAckDrops_ < TEST_DROP_ACK_COUNT) {
testAckDrops_++;

Serial.print("[RX][TEST] Simulating lost ACK ");
Serial.print(testAckDrops_);
Serial.print("/");
Serial.println(TEST_DROP_ACK_COUNT);

radioRecovery_.startReceiveWithRecovery();
return;
}
RadioMessage ack;

ack.type = RADIO_MSG_ACK;
ack.senderId = RX_NODE_ID;
ack.destinationId = TX_NODE_ID;
ack.messageId = messageId;
ack.payload = "OK";

String plaintext;
String encryptedFrame;

if (!encodeRadioMessage(ack, plaintext) ||
!encryptPacket(plaintext, encryptedFrame)) {

Serial.println("[RX] ACK encoding/encryption failed!");

radioRecovery_.startReceiveWithRecovery();
return;
}

radio.standby();
delay(5);

Serial.print("[RX] Sending ACK for messageId=");
Serial.println(messageId);

operationDone = false;

transmissionState_ = radio.startTransmit(encryptedFrame);

if (transmissionState_ == RADIOLIB_ERR_NONE) {
transmitFlag_ = true;

ackTxStartedAt_ = millis();
ackTransmissionsStarted_++;

} else {
ackTransmissionFailures_++;

Serial.print("[RX] startTransmit failed: ");
Serial.println(transmissionState_);

transmitFlag_ = false;

radioRecovery_.startReceiveWithRecovery();
}
}


// Start channel search when requested by duplicate detection
// or when no DATA has arrived for a configured period.
void RxController::startChannelSearch() {
if (radioChannelManager.isSearching() ||
transmitFlag_ ||
radioRecovery_.isRecovering()) {
return;
}

Serial.println("[RX][CHANNEL] Starting channel search");

if (!radioChannelManager.startSearch(RX_SCAN_DWELL_MS)) {
Serial.println("[RX][CHANNEL] Cannot start search");

// Avoid retrying a failed search on every update() iteration.
scanAfterAck_ = false;
lastDataAt_ = millis();

return;
}

scanAfterAck_ = false;

// The manager has already switched to the first search channel.
radioRecovery_.startReceiveWithRecovery();

Serial.print("[RX][CHANNEL] Listening on channel ");
Serial.println(radioChannelManager.currentChannel());
}


// Maintain the channel search state.
void RxController::updateChannelSearch() {
if (transmitFlag_ || radioRecovery_.isRecovering()) {
return;
}

// Start scanning after the ACK has finished transmitting.
// If the test deliberately dropped the ACK, scanning can begin
// immediately because the radio is already in receive mode.
if (scanAfterAck_) {
startChannelSearch();
return;
}

// Start scanning if no valid DATA has arrived for too long.
if (!radioChannelManager.isSearching()) {
if ((unsigned long)(millis() - lastDataAt_) >=
RX_SCAN_AFTER_IDLE_MS) {

startChannelSearch();
}

return;
}

// Switch to the next channel after the listening interval.
if (radioChannelManager.updateSearch()) {
Serial.print("[RX][CHANNEL] Listening on channel ");
Serial.println(radioChannelManager.currentChannel());

radioRecovery_.startReceiveWithRecovery();
}
}


void RxController::printPeriodicStats() {
if ((unsigned long)(millis() - lastStatsAt_) <
RX_STATS_INTERVAL_MS) {
return;
}

lastStatsAt_ = millis();

Serial.println("[RX] === PERIODIC STATISTICS ===");

Serial.print("DATA frames received: ");
Serial.println(receivedDataFrames_);

Serial.print("New application messages: ");
Serial.println(newDataMessages_);

Serial.print("Duplicate DATA frames: ");
Serial.println(duplicateDataFrames_);

Serial.print("ACK transmissions started: ");
Serial.println(ackTransmissionsStarted_);

Serial.print("ACK transmissions completed: ");
Serial.println(ackTransmissionsCompleted_);

Serial.print("ACK transmission failures: ");
Serial.println(ackTransmissionFailures_);

Serial.print("Decryption/replay errors: ");
Serial.println(decryptFailures_);

Serial.print("startReceive failures: ");
Serial.println(radioRecovery_.receiveStartFailures());

Serial.print("Successful radio recoveries: ");
Serial.println(radioRecovery_.successfulRecoveries());

Serial.print("Failed radio recovery attempts: ");
Serial.println(radioRecovery_.failedRecoveryAttempts());

Serial.print("Current channel: ");
Serial.println(radioChannelManager.currentChannel());

Serial.print("Channel search active: ");
Serial.println(
radioChannelManager.isSearching() ? "YES" : "NO"
);
}