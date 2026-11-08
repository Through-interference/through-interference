#include "tx/TxOutbox.h"

#include "crypto.h"
#include "protocol.h"
#include "radio_config.h"


bool TxOutbox::begin() {
    ready_ = prefs_.begin("rad_txmsg", false);

    Serial.print("[TX][NVS] Namespace open: ");
    Serial.println(ready_ ? "OK" : "FAILED");

    return ready_;
}

bool TxOutbox::persistRecord() {
    if (!ready_) return false;

    // Write the record first. The active marker is the commit marker.
    if (prefs_.putUInt("out_mid", message_.messageId) != sizeof(uint32_t)) return false;
    if (prefs_.putUInt("out_src", message_.senderId) != sizeof(uint32_t)) return false;
    if (prefs_.putUInt("out_dst", message_.destinationId) != sizeof(uint32_t)) return false;
    if (prefs_.putString("out_pay", message_.payload) == 0) return false;
    if (prefs_.putUChar("out_state", 1) != sizeof(uint8_t)) return false; // WAIT_ACK
    if (prefs_.putBool("active", true) != sizeof(bool)) return false;

    active_ = true;
    return true;
}

bool TxOutbox::allocateMessageId() {
    if (!ready_) {
        Serial.println("[TX][NVS] Preferences not ready!");
        return false;
    }

    uint32_t previous = prefs_.getUInt("last_msg", 0);

    Serial.print("[TX][NVS] Previous ID: ");
    Serial.println(previous);

    if (previous == UINT32_MAX) {
        Serial.println("[TX][NVS] ID exhausted!");
        return false;
    }

    uint32_t next = previous + 1;

    size_t written = prefs_.putUInt("last_msg", next);

    Serial.print("[TX][NVS] Bytes written: ");
    Serial.println(written);

    uint32_t saved = prefs_.getUInt("last_msg", 0);

    Serial.print("[TX][NVS] Readback ID: ");
    Serial.println(saved);

    if (written != sizeof(next) || saved != next) {
        Serial.println("[TX][NVS] WRITE FAILED!");
        return false;
    }

    message_.messageId = next;

    Serial.println("[TX][NVS] ID allocated successfully");
    return true;
}

bool TxOutbox::buildEncryptedTransaction() {
    RadioMessage message;
    message.type = RADIO_MSG_DATA;
    message.senderId = message_.senderId;
    message.destinationId = message_.destinationId;
    message.messageId = message_.messageId;
    message.payload = message_.payload;

    String plaintext;
    if (!encodeRadioMessage(message, plaintext)) {
        Serial.println("[TX] Application message encoding failed");
        return false;
    }

    // A restored transaction gets a fresh crypto counter at boot.
    // Retries during this boot reuse this exact encrypted frame.
    encryptedFrame_ = "";
    if (!encryptPacket(plaintext, encryptedFrame_)) {
        Serial.println("[TX] Encryption failed");
        return false;
    }

    Serial.print("[TX] Logical message ID: ");
    Serial.println(message_.messageId);
    Serial.print("[TX] Application plaintext: ");
    Serial.println(plaintext);
    return true;
}

void TxOutbox::fatal(const char* message) {
    Serial.println(message);
    while (true) delay(1000);
}

bool TxOutbox::restore() {
    if (!prefs_.getBool("active", false)) {
        return false;
    }

    if (prefs_.getUChar("out_state", 0) != 1) {
        fatal("[TX][FATAL] Invalid outbox state");
    }

    message_.messageId = prefs_.getUInt("out_mid", 0);
    message_.senderId = prefs_.getUInt("out_src", 0);
    message_.destinationId = prefs_.getUInt("out_dst", 0);
    message_.payload = prefs_.getString("out_pay", "");

    if (message_.messageId == 0 ||
        message_.senderId != TX_NODE_ID ||
        message_.destinationId != RX_NODE_ID ||
        message_.payload.length() == 0) {
        fatal("[TX][FATAL] Invalid persistent outbox record");
    }

    // патом убрать!!!!!!
    if (message_.payload.startsWith("TEST_SEQUENCE=")) {
        Serial.println("[TX] Removing obsolete test message");

        if (prefs_.putBool("active", false) != sizeof(bool)) {
            fatal("[TX][FATAL] Cannot clear test message");
        }

        active_ = false;
        return false;
    }

    active_ = true;
    Serial.print("[TX] Restored pending command, messageId=");
    Serial.println(message_.messageId);

    if (!buildEncryptedTransaction()) {
        fatal("[TX][FATAL] Cannot restore encrypted transaction");
    }
    return true;
}

bool TxOutbox::create(const String& customPayload) {
    if (!ready_ || active_ || customPayload.length() == 0) {
        return false;
    }

    if (!allocateMessageId()) {
        return false;
    }

    message_.senderId = TX_NODE_ID;
    message_.destinationId = RX_NODE_ID;
    message_.payload = customPayload;

    if (!persistRecord()) {
        fatal("[TX][FATAL] Cannot persist drone command");
    }

    if (!buildEncryptedTransaction()) {
        fatal("[TX][FATAL] Cannot encrypt drone command");
    }

    Serial.print("[TX] Drone command queued: ");
    Serial.println(customPayload);

    return true;
}

bool TxOutbox::acknowledge() {
    if (!active_ || !ready_) return false;

    if (prefs_.putBool("active", false) != sizeof(bool)) {
        Serial.println("[TX] Cannot clear outbox marker");
        return false;
    }

    active_ = false;
    return true;
}

bool TxOutbox::isActive() const {
    return active_;
}

uint32_t TxOutbox::messageId() const {
    return message_.messageId;
}

const String& TxOutbox::encryptedFrame() const {
    return encryptedFrame_;
}
