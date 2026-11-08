#pragma once

#include <Arduino.h>
#include <stdint.h>

// The whole encoded message is encrypted/authenticated by crypto.h (AES-GCM).
// Wire plaintext format before encryption:
// V1|DATA|sender_id|destination_id|message_id|payload
// V1|ACK|sender_id|destination_id|message_id|payload

enum RadioMessageType : uint8_t {
    RADIO_MSG_INVALID = 0,
    RADIO_MSG_DATA = 1,
    RADIO_MSG_ACK = 2,
    RADIO_MSG_HEARTBEAT = 3
};

struct RadioMessage {
    RadioMessageType type = RADIO_MSG_INVALID;
    uint32_t senderId = 0;
    uint32_t destinationId = 0;
    uint32_t messageId = 0;
    String payload;
};

inline bool protocolParseUInt32(const String& text, uint32_t& value) {
    if (text.length() == 0) return false;

    uint64_t parsed = 0;
    for (size_t i = 0; i < text.length(); ++i) {
        const char c = text.charAt(i);
        if (c < '0' || c > '9') return false;

        parsed = parsed * 10ULL + static_cast<uint32_t>(c - '0');
        if (parsed > UINT32_MAX) return false;
    }

    value = static_cast<uint32_t>(parsed);
    return true;
}

inline bool encodeRadioMessage(const RadioMessage& message, String& output) {
    output = "";

    const bool isHeartbeat =
    message.type == RADIO_MSG_HEARTBEAT;

    if ((message.type != RADIO_MSG_DATA &&
        message.type != RADIO_MSG_ACK &&
        !isHeartbeat) ||
        message.senderId == 0 ||
        message.destinationId == 0 ||
        (isHeartbeat ? message.messageId != 0
                    : message.messageId == 0) ||
        message.payload.length() == 0) {
        return false;
    }

    output.reserve(48 + message.payload.length());
    output = "V1|";
    if (message.type == RADIO_MSG_DATA) {
        output += "DATA|";
    } else if (message.type == RADIO_MSG_ACK) {
        output += "ACK|";
    } else {
        output += "HB|";
    }
    output += String(message.senderId);
    output += '|';
    output += String(message.destinationId);
    output += '|';
    output += String(message.messageId);
    output += '|';
    output += message.payload;
    return true;
}

inline bool decodeRadioMessage(const String& input, RadioMessage& message) {
    message.type = RADIO_MSG_INVALID;
    message.senderId = 0;
    message.destinationId = 0;
    message.messageId = 0;
    message.payload = "";

    if (!input.startsWith("V1|")) return false;

    // Split only the five header separators. Payload may itself contain '|'.
    const int p1 = input.indexOf('|');
    const int p2 = input.indexOf('|', p1 + 1);
    const int p3 = input.indexOf('|', p2 + 1);
    const int p4 = input.indexOf('|', p3 + 1);
    const int p5 = input.indexOf('|', p4 + 1);

    if (p1 != 2 || p2 < 0 || p3 < 0 || p4 < 0 || p5 < 0) return false;
    if (input.substring(0, p1) != "V1") return false;

    const String typeText = input.substring(p1 + 1, p2);
    if (typeText == "DATA") {
        message.type = RADIO_MSG_DATA;
    } else if (typeText == "ACK") {
        message.type = RADIO_MSG_ACK;
    } else if (typeText == "HB") {
        message.type = RADIO_MSG_HEARTBEAT;
    } else {
        return false;
    }

    if (!protocolParseUInt32(input.substring(p2 + 1, p3), message.senderId) ||
        !protocolParseUInt32(input.substring(p3 + 1, p4), message.destinationId) ||
        !protocolParseUInt32(input.substring(p4 + 1, p5), message.messageId)) {
        message.type = RADIO_MSG_INVALID;
        return false;
    }

    message.payload = input.substring(p5 + 1);
    
    const bool isHeartbeat =
        message.type == RADIO_MSG_HEARTBEAT;

    if (message.senderId == 0 ||
        message.destinationId == 0 ||
        (isHeartbeat ? message.messageId != 0
                    : message.messageId == 0) ||
        message.payload.length() == 0) {

        message.type = RADIO_MSG_INVALID;
        return false;
    }

    return true;
}
