#pragma once

#include <Arduino.h>
#include <Preferences.h>

struct TxMessage {
    uint32_t sequence = 1;
    uint32_t messageId = 0;
    uint32_t senderId = 0;
    uint32_t destinationId = 0;
    String payload;
};

// Persistent storage for the message currently waiting for ACK.
class TxOutbox {
public:
    bool begin();
    bool restore();
    bool create();
    bool acknowledge();

    bool isActive() const;
    uint32_t sequence() const;
    uint32_t messageId() const;
    const String& encryptedFrame() const;

private:
    Preferences prefs_;
    bool ready_ = false;
    bool active_ = false;
    TxMessage message_;
    String encryptedFrame_;

    bool persistRecord();
    bool allocateMessageId();
    bool buildEncryptedTransaction();
    void fatal(const char* message);
};
