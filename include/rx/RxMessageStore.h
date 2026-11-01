#pragma once

#include <Arduino.h>
#include <Preferences.h>
#include "protocol.h"

// Stores the last committed application transaction in two NVS slots.
class RxMessageStore {
public:
    bool begin();
    bool load();
    bool save(const RadioMessage& message);

    bool hasMessage() const;
    uint32_t lastMessageId() const;
    uint32_t lastSourceId() const;
    uint32_t lastDestinationId() const;
    const String& lastPayload() const;

private:
    Preferences prefs_;
    bool ready_ = false;
    bool hasMessage_ = false;
    uint8_t activeSlot_ = 0;

    uint32_t messageId_ = 0;
    uint32_t sourceId_ = 0;
    uint32_t destinationId_ = 0;
    String payload_;

    void makeSlotKeys(uint8_t slot, char* id, char* source,
                      char* destination, char* payload) const;
};
