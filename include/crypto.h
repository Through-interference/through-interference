#pragma once

#include <Arduino.h>
#include <stdint.h>

enum CryptoPacketStatus {
    CRYPTO_PACKET_ERROR,
    CRYPTO_PACKET_NEW,
    CRYPTO_PACKET_DUPLICATE,
    CRYPTO_PACKET_REPLAY
};

bool cryptoInit();

bool cryptoEncryptBytes(
    const uint8_t* payload,
    size_t payloadLength,
    String& frame
);

CryptoPacketStatus cryptoDecryptBytes(
    const String& frame,
    uint8_t* payloadOut,
    size_t payloadCapacity,
    size_t& payloadLength,
    uint32_t& senderIdOut,
    uint64_t& counterOut
);

bool encryptPacket(const String& plain, String& frame);

CryptoPacketStatus decryptPacketEx(
    const String& frame,
    String& plain,
    uint32_t& senderId,
    uint64_t& counter
);

bool decryptPacket(const String& frame, String& plain);
