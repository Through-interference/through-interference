#include "rx/RxMessageStore.h"

#include <stdio.h>
#include "radio_config.h"

bool RxMessageStore::begin() {
    ready_ = prefs_.begin("rad_rxmsg", false);
    return ready_;
}

void RxMessageStore::makeSlotKeys(
    uint8_t slot,
    char* id,
    char* source,
    char* destination,
    char* payload
) const {
    const char prefix = (slot == 0) ? 'a' : 'b';
    snprintf(id, 12, "%c_id", prefix);
    snprintf(source, 12, "%c_src", prefix);
    snprintf(destination, 12, "%c_dst", prefix);
    snprintf(payload, 12, "%c_pay", prefix);
}

bool RxMessageStore::load() {
    if (!ready_) return false;

    hasMessage_ = prefs_.getBool("has_msg", false);
    if (!hasMessage_) {
        // Migrate the previous single-record format when it exists.
        const uint32_t legacyId = prefs_.getUInt("last_id", 0);
        if (legacyId == 0) return true;

        RadioMessage legacy;
        legacy.type = RADIO_MSG_DATA;
        legacy.senderId = prefs_.getUInt("last_src", TX_NODE_ID);
        legacy.destinationId = RX_NODE_ID;
        legacy.messageId = legacyId;
        legacy.payload = prefs_.getString("last_pay", "");

        if (legacy.senderId == 0 || legacy.payload.length() == 0) {
            Serial.println("[RX][FATAL] Legacy transaction record is invalid; refusing to overwrite it");
            return false;
        }
        if (!save(legacy)) {
            Serial.println("[RX][FATAL] Failed to migrate legacy transaction record");
            return false;
        }
        Serial.println("[RX] Migrated previous single-record transaction state to double-buffered NVS");
        return true;
    }

    activeSlot_ = prefs_.getUChar("active_slot", 0);
    if (activeSlot_ > 1) {
        Serial.println("[RX][FATAL] Invalid active transaction slot in NVS");
        return false;
    }

    char idKey[12], sourceKey[12], destinationKey[12], payloadKey[12];
    makeSlotKeys(activeSlot_, idKey, sourceKey, destinationKey, payloadKey);

    messageId_ = prefs_.getUInt(idKey, 0);
    sourceId_ = prefs_.getUInt(sourceKey, 0);
    destinationId_ = prefs_.getUInt(destinationKey, 0);
    payload_ = prefs_.getString(payloadKey, "");

    if (messageId_ == 0 || sourceId_ == 0 || destinationId_ == 0 ||
        payload_.length() == 0) {
        Serial.println("[RX][FATAL] Active transaction slot is incomplete; refusing to overwrite state");
        return false;
    }

    Serial.print("[RX] Restored last committed transaction: source=");
    Serial.print(sourceId_);
    Serial.print(" messageId=");
    Serial.println(messageId_);
    return true;
}

bool RxMessageStore::save(const RadioMessage& message) {
    if (!ready_ || message.messageId == 0 || message.senderId == 0 ||
        message.destinationId == 0 || message.payload.length() == 0) {
        return false;
    }

    // Write to the inactive slot. Switch the active-slot marker last.
    const uint8_t targetSlot = hasMessage_ ? static_cast<uint8_t>(1 - activeSlot_) : 0;
    char idKey[12], sourceKey[12], destinationKey[12], payloadKey[12];
    makeSlotKeys(targetSlot, idKey, sourceKey, destinationKey, payloadKey);

    if (prefs_.putUInt(idKey, message.messageId) != sizeof(uint32_t)) return false;
    if (prefs_.putUInt(sourceKey, message.senderId) != sizeof(uint32_t)) return false;
    if (prefs_.putUInt(destinationKey, message.destinationId) != sizeof(uint32_t)) return false;
    if (prefs_.putString(payloadKey, message.payload) == 0) return false;

    if (prefs_.putUChar("active_slot", targetSlot) != sizeof(uint8_t)) return false;
    if (!hasMessage_ && prefs_.putBool("has_msg", true) != sizeof(bool)) return false;

    hasMessage_ = true;
    activeSlot_ = targetSlot;
    payload_ = message.payload;
    sourceId_ = message.senderId;
    destinationId_ = message.destinationId;
    messageId_ = message.messageId;
    return true;
}

bool RxMessageStore::hasMessage() const { return hasMessage_; }
uint32_t RxMessageStore::lastMessageId() const { return messageId_; }
uint32_t RxMessageStore::lastSourceId() const { return sourceId_; }
uint32_t RxMessageStore::lastDestinationId() const { return destinationId_; }
const String& RxMessageStore::lastPayload() const { return payload_; }
