#include "crypto.h"

#include <Arduino.h>
#include <Preferences.h>
#include <mbedtls/gcm.h>
#include <stdint.h>
#include <string.h>
#include "crypto_secrets.h"
#include "AtCommandManager.h"

// Максимум полезных данных в одном пакете.
// Кадр кодируется шестнадцатеричными символами.
static const size_t CRYPTO_MAX_PLAINTEXT = 96;

// ============================================================
// ВНУТРЕННЕЕ СОСТОЯНИЕ
// ============================================================

namespace crypto_detail {

static Preferences prefs;
static mbedtls_gcm_context gcm;

static bool ready = false;

// txCounter — последний счётчик, использованный в этой загрузке.
// В NVS по ключу txctr хранится ВЕРХНЯЯ ГРАНИЦА зарезервированного блока.
static uint64_t txCounter = 0;
static uint64_t txBlockEnd = 0;
static uint64_t lastRxCounter = 0;
static uint64_t lastPersistedRxCounter = 0;

// При резервировании сохраняем сразу 32 значения.
// После перезапуска неиспользованный остаток блока пропускается.
static const uint64_t TX_COUNTER_BLOCK_SIZE = 32ULL;

static const size_t NONCE_SIZE = 12;
static const size_t TAG_SIZE = 16;
static const size_t AAD_SIZE = 13;

static const char HEX_DIGITS[] = "0123456789ABCDEF";

// Резервируем новый диапазон TX-счётчиков в NVS.
// Возвращает false, если запись не удалась или есть риск переполнения.
static bool reserveTxBlock() {
    if (txCounter > UINT64_MAX - TX_COUNTER_BLOCK_SIZE) {
        return false;
    }

    uint64_t newEnd = txCounter + TX_COUNTER_BLOCK_SIZE;

    if (prefs.putULong64("txctr", newEnd) != sizeof(newEnd)) {
        return false;
    }

    txBlockEnd = newEnd;
    return true;
}

static int hexNibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

static void appendUInt32Hex(String &out, uint32_t value) {
    for (int shift = 28; shift >= 0; shift -= 4) {
        out += HEX_DIGITS[(value >> shift) & 0x0F];
    }
}

static void appendUInt64Hex(String &out, uint64_t value) {
    for (int shift = 60; shift >= 0; shift -= 4) {
        out += HEX_DIGITS[(value >> shift) & 0x0F];
    }
}

static bool readUInt32Hex(const String &s, int start, uint32_t &value) {
    value = 0;

    for (int i = 0; i < 8; i++) {
        int digit = hexNibble(s.charAt(start + i));
        if (digit < 0) return false;
        value = (value << 4) | (uint32_t)digit;
    }

    return true;
}

static bool readUInt64Hex(const String &s, int start, uint64_t &value) {
    value = 0;

    for (int i = 0; i < 16; i++) {
        int digit = hexNibble(s.charAt(start + i));
        if (digit < 0) return false;
        value = (value << 4) | (uint64_t)digit;
    }

    return true;
}

static bool hexToBytes(
    const String &s,
    int start,
    size_t byteCount,
    uint8_t *out
) {
    for (size_t i = 0; i < byteCount; i++) {
        int high = hexNibble(s.charAt(start + 2 * i));
        int low  = hexNibble(s.charAt(start + 2 * i + 1));

        if (high < 0 || low < 0) return false;
        out[i] = (uint8_t)((high << 4) | low);
    }

    return true;
}

// NONCE = ID отправителя (4 байта) + счётчик (8 байт).
// Оба значения записываются в big-endian.
static void makeNonce(
    uint32_t nodeId,
    uint64_t counter,
    uint8_t nonce[NONCE_SIZE]
) {
    nonce[0] = (nodeId >> 24) & 0xFF;
    nonce[1] = (nodeId >> 16) & 0xFF;
    nonce[2] = (nodeId >> 8) & 0xFF;
    nonce[3] = nodeId & 0xFF;

    for (int i = 0; i < 8; i++) {
        nonce[4 + i] = (uint8_t)(counter >> (56 - 8 * i));
    }
}

// Дополнительно аутентифицируем версию кадра и nonce.
static void makeAAD(
    const uint8_t nonce[NONCE_SIZE],
    uint8_t aad[AAD_SIZE]
) {
    aad[0] = 0xE1;
    memcpy(aad + 1, nonce, NONCE_SIZE);
}

static void appendBytesHex(
    String &out,
    const uint8_t *data,
    size_t length
) {
    for (size_t i = 0; i < length; i++) {
        out += HEX_DIGITS[data[i] >> 4];
        out += HEX_DIGITS[data[i] & 0x0F];
    }
}

} // namespace crypto_detail

// ============================================================
// ИНИЦИАЛИЗАЦИЯ
// ============================================================
bool cryptoInit() {
    using namespace crypto_detail;

    if (ready) return true;

    if (!prefs.begin("cryptolink", false)) {
        return false;
    }

    txCounter = prefs.getULong64("txctr", 0);
    txBlockEnd = txCounter;
    lastRxCounter = prefs.getULong64("rxctr", 0);
    lastPersistedRxCounter = lastRxCounter;

    mbedtls_gcm_init(&gcm);

    // Проверяем, есть ли сохраненный ключ в NVS от AtCommandManager (профиль bind.py)
    Preferences cfgPrefs;
    uint8_t activeKey[16];
    bool hasCustomKey = false;

    if (cfgPrefs.begin("modem_cfg", true)) {
        if (cfgPrefs.isKey("cfg_blob")) {
            ModemConfig tempCfg;
            cfgPrefs.getBytes("cfg_blob", &tempCfg, sizeof(ModemConfig));
            // Проверяем, не нулевой ли ключ
            for (int i = 0; i < 16; i++) {
                if (tempCfg.aesKey[i] != 0) {
                    hasCustomKey = true;
                    break;
                }
            }
            if (hasCustomKey) {
                memcpy(activeKey, tempCfg.aesKey, 16);
            }
        }
        cfgPrefs.end();
    }

    // Если через bind.py ключ еще не задавался, берем заглушку и предупреждаем
    if (!hasCustomKey) {
        Serial.println("[CRITICAL] WARNING: No AES key found in NVS! Run bind.py to configure encryption.");
        memcpy(activeKey, DEFAULT_UNBOUND_AES_KEY, 16);
    } else {
        Serial.println("[CRYPTO] Loaded secure AES-128 key from NVS profile.");
    }

    int result = mbedtls_gcm_setkey(
        &gcm,
        MBEDTLS_CIPHER_ID_AES,
        activeKey,
        128
    );

    if (result != 0) {
        mbedtls_gcm_free(&gcm);
        prefs.end();
        return false;
    }

    if (!reserveTxBlock()) {
        mbedtls_gcm_free(&gcm);
        prefs.end();
        return false;
    }

    ready = true;
    return true;
}

// ============================================================
// ШИФРОВАНИЕ ПРОИЗВОЛЬНЫХ БАЙТОВ
// Вход: payload + длина.
// Выход: текстовый радиокадр в frame.
// ============================================================

bool cryptoEncryptBytes(
    const uint8_t *payload,
    size_t payloadLength,
    String &frame
) {
    using namespace crypto_detail;

    frame = "";

    if (!ready || payload == nullptr) return false;

    if (payloadLength == 0 || payloadLength > CRYPTO_MAX_PLAINTEXT) {
        return false;
    }

    // Не допускаем переполнения счётчика.
    if (txCounter == UINT64_MAX) {
        return false;
    }

    // Если текущий блок закончился, резервируем следующий блок в NVS.
    if (txCounter >= txBlockEnd) {
        if (!reserveTxBlock()) {
            return false;
        }
    }

    uint64_t nextCounter = txCounter + 1;
    txCounter = nextCounter;

    uint8_t nonce[NONCE_SIZE];
    uint8_t aad[AAD_SIZE];
    uint8_t ciphertext[CRYPTO_MAX_PLAINTEXT];
    uint8_t tag[TAG_SIZE];

    makeNonce((uint32_t)CRYPTO_NODE_ID, nextCounter, nonce);
    makeAAD(nonce, aad);

    int result = mbedtls_gcm_crypt_and_tag(
        &gcm,
        MBEDTLS_GCM_ENCRYPT,
        payloadLength,
        nonce,
        NONCE_SIZE,
        aad,
        AAD_SIZE,
        payload,
        ciphertext,
        TAG_SIZE,
        tag
    );

    if (result != 0) {
        // Счётчик не откатываем: его повторное использование не нужно.
        return false;
    }

    // Формат кадра:
    // E1|SENDER_ID|COUNTER|CIPHERTEXT_HEX|TAG_HEX
    // Максимум 254 символа при payloadLength == 96.
    frame.reserve(62 + 2 * payloadLength);

    frame = "E1|";
    appendUInt32Hex(frame, (uint32_t)CRYPTO_NODE_ID);
    frame += '|';

    appendUInt64Hex(frame, nextCounter);
    frame += '|';

    appendBytesHex(frame, ciphertext, payloadLength);
    frame += '|';

    appendBytesHex(frame, tag, TAG_SIZE);

    return frame.length() == 62 + 2 * payloadLength;
}

// ============================================================
// РАСШИФРОВАНИЕ ПРОИЗВОЛЬНЫХ БАЙТОВ
// ============================================================

CryptoPacketStatus cryptoDecryptBytes(
    const String &frame,
    uint8_t *payloadOut,
    size_t payloadCapacity,
    size_t &payloadLength,
    uint32_t &senderIdOut,
    uint64_t &counterOut,
    bool persistCounter
) {
    using namespace crypto_detail;

    payloadLength = 0;
    senderIdOut = 0;
    counterOut = 0;

    if (!ready || payloadOut == nullptr) {
        return CRYPTO_PACKET_ERROR;
    }

    if (frame.length() < 62 || frame.length() > 255 ||
        !frame.startsWith("E1|")) {
        return CRYPTO_PACKET_ERROR;
    }

    int p1 = frame.indexOf('|');
    int p2 = frame.indexOf('|', p1 + 1);
    int p3 = frame.indexOf('|', p2 + 1);
    int p4 = frame.indexOf('|', p3 + 1);

    if (p1 != 2 || p2 < 0 || p3 < 0 || p4 < 0) {
        return CRYPTO_PACKET_ERROR;
    }

    // ID занимает 8 hex-символов, счётчик — 16, тег — 32.
    if (p2 - p1 - 1 != 8 ||
        p3 - p2 - 1 != 16 ||
        frame.length() - p4 - 1 != 32) {
        return CRYPTO_PACKET_ERROR;
    }

    int cipherHexLength = p4 - p3 - 1;
    if (cipherHexLength <= 0 || (cipherHexLength % 2) != 0) {
        return CRYPTO_PACKET_ERROR;
    }

    size_t cipherLength = (size_t)cipherHexLength / 2;
    if (cipherLength > CRYPTO_MAX_PLAINTEXT ||
        cipherLength > payloadCapacity) {
        return CRYPTO_PACKET_ERROR;
    }

    uint32_t senderId;
    uint64_t counter;

    if (!readUInt32Hex(frame, p1 + 1, senderId) ||
        !readUInt64Hex(frame, p2 + 1, counter)) {
        return CRYPTO_PACKET_ERROR;
    }

    // Принимаем только настроенного партнёра.
    if (senderId != (uint32_t)CRYPTO_PEER_ID || counter == 0) {
        return CRYPTO_PACKET_ERROR;
    }

    uint8_t ciphertext[CRYPTO_MAX_PLAINTEXT];
    uint8_t tag[TAG_SIZE];
    uint8_t nonce[NONCE_SIZE];
    uint8_t aad[AAD_SIZE];
    uint8_t plaintext[CRYPTO_MAX_PLAINTEXT];

    if (!hexToBytes(frame, p3 + 1, cipherLength, ciphertext) ||
        !hexToBytes(frame, p4 + 1, TAG_SIZE, tag)) {
        return CRYPTO_PACKET_ERROR;
    }

    makeNonce(senderId, counter, nonce);
    makeAAD(nonce, aad);

    // auth_decrypt одновременно расшифровывает и проверяет тег GCM.
    int result = mbedtls_gcm_auth_decrypt(
        &gcm,
        cipherLength,
        nonce,
        NONCE_SIZE,
        aad,
        AAD_SIZE,
        tag,
        TAG_SIZE,
        ciphertext,
        plaintext
    );

    if (result != 0) {
        return CRYPTO_PACKET_ERROR;
    }

    senderIdOut = senderId;
    counterOut = counter;

    // Проверяем повторы после успешной криптографической проверки.
    if (counter < lastRxCounter) {
        return CRYPTO_PACKET_REPLAY;
    }

    if (counter == lastRxCounter) {

        if (persistCounter &&
            !cryptoCommitRxCounter(counter)) {
            return CRYPTO_PACKET_ERROR;
        }

        memcpy(payloadOut, plaintext, cipherLength);
        payloadLength = cipherLength;

        return CRYPTO_PACKET_DUPLICATE;
    }

    lastRxCounter = counter;

    if (persistCounter &&
        !cryptoCommitRxCounter(counter)) {
        return CRYPTO_PACKET_ERROR;
    }

    memcpy(payloadOut, plaintext, cipherLength);
    payloadLength = cipherLength;

    return CRYPTO_PACKET_NEW;
}

// ============================================================
// УДОБНЫЕ ФУНКЦИИ ДЛЯ ТЕКСТОВЫХ КОМАНД
// ============================================================

bool encryptPacket(const String &plain, String &frame) {
    return cryptoEncryptBytes(
        (const uint8_t *)plain.c_str(),
        plain.length(),
        frame
    );
}

CryptoPacketStatus decryptPacketEx(
    const String &frame,
    String &plain,
    uint32_t &senderId,
    uint64_t &counter,
    bool persistCounter
) {
    uint8_t buffer[CRYPTO_MAX_PLAINTEXT];
    size_t length = 0;

    CryptoPacketStatus status = cryptoDecryptBytes(
        frame,
        buffer,
        sizeof(buffer),
        length,
        senderId,
        counter,
        persistCounter
    );

    plain = "";

    if (status != CRYPTO_PACKET_NEW &&
        status != CRYPTO_PACKET_DUPLICATE) {
        return status;
    }

    // Эта обёртка предназначена для текстовых команд.
    // Для бинарной телеметрии используй cryptoDecryptBytes().
    for (size_t i = 0; i < length; i++) {
        if (buffer[i] == 0) {
            plain = "";
            return CRYPTO_PACKET_ERROR;
        }
    }

    plain.reserve(length);
    for (size_t i = 0; i < length; i++) {
        plain += (char)buffer[i];
    }

    return status;
}

// true возвращается только для нового принятого пакета.
// Для обработки повторов используй decryptPacketEx().
bool decryptPacket(const String &frame, String &plain) {
    uint32_t senderId = 0;
    uint64_t counter = 0;

    return decryptPacketEx(frame, plain, senderId, counter) ==
           CRYPTO_PACKET_NEW;
}

bool cryptoUpdateKey(const uint8_t newKey[16]) {
    using namespace crypto_detail;

    if (!ready || newKey == nullptr) {
        return false;
    }

    int result = mbedtls_gcm_setkey(
        &gcm,
        MBEDTLS_CIPHER_ID_AES,
        newKey,
        128
    );

    return result == 0;
}

bool cryptoCommitRxCounter(uint64_t counter) {
    using namespace crypto_detail;

    if (!ready ||
        counter == 0 ||
        counter > lastRxCounter) {
        return false;
    }

    if (counter <= lastPersistedRxCounter) {
        return true;
    }

    if (prefs.putULong64("rxctr", counter) != sizeof(counter)) {
        return false;
    }

    lastPersistedRxCounter = counter;
    return true;
}