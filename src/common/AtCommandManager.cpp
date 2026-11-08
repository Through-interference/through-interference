#include "AtCommandManager.h"
#include <driver/twai.h>
#include <errno.h>
#include <stdlib.h>
#include <ctype.h>
#include <math.h>

#define FW_VERSION "2.0.0-CAN"

namespace {

bool isValidModemConfig(const ModemConfig& cfg) {
    if (!isfinite(cfg.primFreqMhz) ||
        !isfinite(cfg.backFreqMhz)) {
        return false;
    }

    if (cfg.primFreqMhz < 850.0f ||
        cfg.primFreqMhz > 930.0f ||
        cfg.backFreqMhz < 850.0f ||
        cfg.backFreqMhz > 930.0f) {
        return false;
    }

    if (cfg.txPowerDbm < -9 || cfg.txPowerDbm > 22) {
        return false;
    }

    switch (cfg.modulation) {
        case classModulation::GFSK100:
        case classModulation::GFSK250:
        case classModulation::LORA_SF7:
        case classModulation::LORA_SF9:
            break;

        default:
            return false;
    }

    if (cfg.tdmaSlot > 3) {
        return false;
    }

    switch (cfg.antMode) {
        case classAntMode::ANT_1:
        case classAntMode::ANT_2:
        case classAntMode::ANT_AUTO:
            break;

        default:
            return false;
    }

    if (cfg.canBaud != 125000 &&
        cfg.canBaud != 250000 &&
        cfg.canBaud != 500000 &&
        cfg.canBaud != 1000000) {
        return false;
    }

    return true;
}

bool parseFloatStrict(String value, float& result) {
    value.trim();

    if (value.length() == 0) {
        return false;
    }

    const char* start = value.c_str();
    char* end = nullptr;

    errno = 0;
    const float parsed = strtof(start, &end);

    if (start == end || errno == ERANGE || !isfinite(parsed)) {
        return false;
    }

    while (*end != '\0' && isspace(static_cast<unsigned char>(*end))) {
        ++end;
    }

    if (*end != '\0') {
        return false;
    }

    result = parsed;
    return true;
}

bool parseLongStrict(String value, long& result) {
    value.trim();

    if (value.length() == 0) {
        return false;
    }

    const char* start = value.c_str();
    char* end = nullptr;

    errno = 0;
    const long parsed = strtol(start, &end, 10);

    if (start == end || errno == ERANGE) {
        return false;
    }

    while (*end != '\0' && isspace(static_cast<unsigned char>(*end))) {
        ++end;
    }

    if (*end != '\0') {
        return false;
    }

    result = parsed;
    return true;
}

}

AtCommandManager::AtCommandManager(Stream& atPort, uint8_t rfSwitchPin)
    : _port(atPort),
      _rfSwitchPin(rfSwitchPin),
      _mutex(nullptr) {
}

void AtCommandManager::begin() {
    if (_mutex == nullptr) {
        _mutex = xSemaphoreCreateMutex();
    }

    if (_mutex == nullptr) {
        _port.println("ERROR: MUTEX_CREATE_FAIL");
        return;
    }

    if (_rfSwitchPin != 255) {
        pinMode(_rfSwitchPin, OUTPUT);
    }

    restoreDefaults();
    loadFromNvs();

    applyAntHardware(_cfg.antMode);

    _rxLine.reserve(128);
}

void AtCommandManager::processInput() {
    if (_mutex == nullptr) {
        return;
    }

    while (_port.available()) {
        char c = static_cast<char>(_port.read());

        if (c == '\n' || c == '\r') {
            if (_lineOverflow) {
                _port.println("ERROR: LINE_TOO_LONG");
                _lineOverflow = false;
                _rxLine = "";
            } else if (_rxLine.length() > 0) {
                handleLine(_rxLine);
                _rxLine = "";
            }
        } else if (_rxLine.length() < 120) {
            _rxLine += c;
        } else {
            _lineOverflow = true;
        }
    }
}

void AtCommandManager::handleLine(String line) {
    line.trim();
    if (_extension && _extension(_context, line, _port)) return;
    String upper = line;
    upper.toUpperCase();

    // 1. Системные команды (База)
    if (upper == "LINK") {
        _port.println("OK");
        return;
    }
    if (upper == "RST") {
        _port.println("OK");
        _port.flush();
        delay(100);
        ESP.restart();
        return;
    }
    if (upper == "VER") {
        _port.printf("+VER: %s (%s %s)\r\nOK\r\n", FW_VERSION, __DATE__, __TIME__);
        return;
    }
    if (upper == "SAVE") {
        if (saveToNvs()) _port.println("OK");
        else _port.println("ERROR: NVS_WRITE_FAIL");
        return;
    }
    if (upper == "RESTORE") {
        restoreDefaults();

        if (!saveToNvs()) {
            _port.println("ERROR: NVS_WRITE_FAIL");
            return;
        }

        retryPendingConfig();

        _port.println("OK");
        return;
    }

    
    // 2. Настройка радиоканала (PHY)
    if (upper.startsWith("PRIMFREQ ")) {
        float freq;

        if (!parseFloatStrict(line.substring(9), freq) ||
            freq < 850.0f ||
            freq > 930.0f) {
            _port.println("ERROR: INVALID_PRIMARY_FREQUENCY");
            return;
        }

        xSemaphoreTake(_mutex, portMAX_DELAY);

        _cfg.primFreqMhz = freq;
        _configDirty = true;

        xSemaphoreGive(_mutex);

        _port.println("OK");
        return;
    }

    if (upper.startsWith("BACKFREQ ")) {
        float freq;

        if (!parseFloatStrict(line.substring(9), freq) ||
            freq < 850.0f ||
            freq > 930.0f) {
            _port.println("ERROR: INVALID_BACKUP_FREQUENCY");
            return;
        }

        xSemaphoreTake(_mutex, portMAX_DELAY);

        _cfg.backFreqMhz = freq;
        _configDirty = true;

        xSemaphoreGive(_mutex);

        _port.println("OK");
        return;
    }

    if (upper.startsWith("PWR ")) {
        long pwr;

        if (!parseLongStrict(line.substring(4), pwr) ||
            pwr < -9 ||
            pwr > 22) {
            _port.println("ERROR: PWR_RANGE_-9_TO_22");
            return;
        }

        xSemaphoreTake(_mutex, portMAX_DELAY);

        _cfg.txPowerDbm = static_cast<int8_t>(pwr);
        _configDirty = true;

        xSemaphoreGive(_mutex);

        _port.println("OK");
        return;
    }

    if (upper.startsWith("MOD ")) {
        String modStr = upper.substring(4);
        classModulation newMod;
        if (modStr == "GFSK100") newMod = classModulation::GFSK100;
        else if (modStr == "GFSK250") newMod = classModulation::GFSK250;
        else if (modStr == "LORA_SF7") newMod = classModulation::LORA_SF7;
        else if (modStr == "LORA_SF9") newMod = classModulation::LORA_SF9;
        else {
            _port.println("ERROR: UNKNOWN_MOD");
            return;
        }
        xSemaphoreTake(_mutex, portMAX_DELAY);
        _cfg.modulation = newMod;
        _configDirty = true;
        xSemaphoreGive(_mutex);
        _port.println("OK");
        return;
    }

    // 3. Сеть и MAC-уровень
    if (upper.startsWith("SLOT ")) {
        long slot;

        if (!parseLongStrict(line.substring(5), slot) ||
            slot < 0 ||
            slot > 3) {
            _port.println("ERROR: SLOT_MUST_BE_0_TO_3");
            return;
        }

        xSemaphoreTake(_mutex, portMAX_DELAY);

        _cfg.tdmaSlot = static_cast<uint8_t>(slot);
        _configDirty = true;

        xSemaphoreGive(_mutex);

        _port.println("OK");
        return;
    }

    if (upper.startsWith("FHSS ")) {
        String val = upper.substring(5);
        if (val != "0" && val != "1") {
            _port.println("ERROR: FHSS_MUST_BE_0_OR_1");
            return;
        }
        xSemaphoreTake(_mutex, portMAX_DELAY);
        _cfg.fhssEnabled = (val == "1");
        _configDirty = true;
        xSemaphoreGive(_mutex);
        _port.println("OK");
        return;
    }

    if (upper.startsWith("KEY ")) {
        String hexKey = line.substring(4);
        uint8_t parsedKey[16];
        if (!parseHexKey(hexKey, parsedKey)) {
            _port.println("ERROR: KEY_REQUIRES_32_HEX_CHARS");
            return;
        }
        xSemaphoreTake(_mutex, portMAX_DELAY);
        memcpy(_cfg.aesKey, parsedKey, 16);
        _configDirty = true;
        xSemaphoreGive(_mutex);
        _port.println("OK");
        return;
    }

    // 4. Управление железом (Hardware)
    if (upper.startsWith("CANBAUD ")) {
        long parsed;
        if (!parseLongStrict(line.substring(8), parsed) || parsed < 0 ||
            !applyCanBaud(static_cast<uint32_t>(parsed))) {
            _port.println("ERROR: UNSUPPORTED_CAN_BAUD");
            return;
        }
        xSemaphoreTake(_mutex, portMAX_DELAY);
        _cfg.canBaud = static_cast<uint32_t>(parsed);
        xSemaphoreGive(_mutex);
        _port.println("OK");
        return;
    }

    // 5. Мониторинг и Телеметрия (Только чтение)
    if (upper == "RSSI") {
        xSemaphoreTake(_mutex, portMAX_DELAY);
        int16_t rssi = _telem.rssiDbm;
        xSemaphoreGive(_mutex);
        _port.printf("+RSSI: %d dBm\r\nOK\r\n", rssi);
        return;
    }
    if (upper == "SNR") {
        xSemaphoreTake(_mutex, portMAX_DELAY);
        float snr = _telem.snrDb;
        xSemaphoreGive(_mutex);
        _port.printf("+SNR: %.1f dB\r\nOK\r\n", snr);
        return;
    }
    if (upper == "STAT") {
        xSemaphoreTake(_mutex, portMAX_DELAY);
        ModemTelemetry t = _telem;
        xSemaphoreGive(_mutex);
        _port.printf("+STAT: TX=%lu, RX=%lu\r\nOK\r\n",
                     t.txPackets, t.rxPackets);
        return;
    }
    if (upper == "DUTY") {
        xSemaphoreTake(_mutex, portMAX_DELAY);
        float duty = _telem.dutyCyclePercent;
        xSemaphoreGive(_mutex);
        _port.printf("+DUTY: %.2f%%\r\nOK\r\n", duty);
        return;
    }

    if (upper.startsWith("CMD:") || upper.startsWith("SEND ")) {
        _port.println("ERROR: TEXT_COMMANDS_UNSUPPORTED_USE_CAN_TX");
        return;
    }

    if (upper == "CFG") {
        xSemaphoreTake(_mutex, portMAX_DELAY);
        float pFreq = _cfg.primFreqMhz;
        float bFreq = _cfg.backFreqMhz;
        int pwr = _cfg.txPowerDbm;
        const char* mod = modToString(_cfg.modulation);
        uint8_t slot = _cfg.tdmaSlot;
        uint8_t fhss = _cfg.fhssEnabled ? 1 : 0;
        xSemaphoreGive(_mutex);

        _port.printf("+CFG: PRIM=%.1f, BACK=%.1f, PWR=%d, MOD=%s, SLOT=%d, FHSS=%d\r\nOK\r\n",
                     pFreq, bFreq, pwr, mod, slot, fhss);
        return;
    }
    _port.println("ERROR: UNKNOWN_COMMAND");
}

// Переключение антенного ключа AS179-92LF
void AtCommandManager::applyAntHardware(classAntMode mode) {
    if (_rfSwitchPin == 255) return;
    if (mode == classAntMode::ANT_1) {
        digitalWrite(_rfSwitchPin, LOW);
    } else if (mode == classAntMode::ANT_2) {
        digitalWrite(_rfSwitchPin, HIGH);
    }
}

bool AtCommandManager::applyCanBaud(uint32_t baud) {
    if (baud != 125000 && baud != 250000 && baud != 500000 && baud != 1000000) {
        return false;
    }
    return _canBaud && _canBaud(_context, baud);
}

bool AtCommandManager::parseHexKey(const String& hexStr, uint8_t* outKey) {
    if (hexStr.length() != 32) return false;
    for (int i = 0; i < 16; i++) {
        char hi = hexStr[i * 2];
        char lo = hexStr[i * 2 + 1];
        auto hexVal = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            return -1;
        };
        int h = hexVal(hi), l = hexVal(lo);
        if (h < 0 || l < 0) return false;
        outKey[i] = (uint8_t)((h << 4) | l);
    }
    return true;
}

void AtCommandManager::restoreDefaults() {
    if (_mutex == nullptr) {
        return;
    }

    xSemaphoreTake(_mutex, portMAX_DELAY);

    _cfg = ModemConfig();
    _configDirty = false;

    xSemaphoreGive(_mutex);
}

bool AtCommandManager::saveToNvs() {
    if (_mutex == nullptr) {
        return false;
    }

    ModemConfig cfgToSave{};

    xSemaphoreTake(_mutex, portMAX_DELAY);
    cfgToSave = _cfg;
    xSemaphoreGive(_mutex);

    if (!isValidModemConfig(cfgToSave)) {
        _port.println("[AT][NVS] Refusing to save invalid configuration");
        return false;
    }

    if (!_prefs.begin("modem_cfg", false)) {
        return false;
    }

    const size_t written = _prefs.putBytes(
        "cfg_blob",
        &cfgToSave,
        sizeof(ModemConfig)
    );

    _prefs.end();

    if (written != sizeof(ModemConfig)) {
        return false;
    }

    return true;
}

void AtCommandManager::loadFromNvs() {
    if (_mutex == nullptr) {
        return;
    }

    if (!_prefs.begin("modem_cfg", true)) {
        _port.println("[AT][NVS] Cannot open configuration namespace");
        return;
    }

    if (!_prefs.isKey("cfg_blob")) {
        _prefs.end();
        _port.println("[AT][NVS] No saved configuration; using defaults");
        return;
    }

    const size_t storedSize = _prefs.getBytesLength("cfg_blob");

    if (storedSize != sizeof(ModemConfig)) {
        _prefs.end();

        _port.println("[AT][NVS] Configuration size mismatch");
        _port.println("[AT][NVS] Re-run bind.py on this board");

        return;
    }

    ModemConfig loadedCfg{};

    const size_t bytesRead = _prefs.getBytes(
        "cfg_blob",
        &loadedCfg,
        sizeof(ModemConfig)
    );

    _prefs.end();

    if (bytesRead != sizeof(ModemConfig)) {
        _port.println("[AT][NVS] Configuration read failed");
        return;
    }

    if (!isValidModemConfig(loadedCfg)) {
        _port.println("[AT][NVS] Configuration validation failed");
        return;
    }

    xSemaphoreTake(_mutex, portMAX_DELAY);

    _cfg = loadedCfg;
    _configDirty = true;

    xSemaphoreGive(_mutex);

    _port.println("[AT][NVS] Configuration loaded successfully");
}

bool AtCommandManager::takePendingConfig(ModemConfig& outCfg) {
    if (_mutex == nullptr) {
        return false;
    }

    bool hasNew = false;

    xSemaphoreTake(_mutex, portMAX_DELAY);

    if (_configDirty) {
        outCfg = _cfg;
        _configDirty = false;
        hasNew = true;
    }

    xSemaphoreGive(_mutex);

    return hasNew;
}

void AtCommandManager::updateTelemetry(const ModemTelemetry& newTelem) {
    if (_mutex == nullptr) {
        return;
    }

    xSemaphoreTake(_mutex, portMAX_DELAY);

    _telem = newTelem;

    xSemaphoreGive(_mutex);
}

void AtCommandManager::retryPendingConfig() {
    if (_mutex == nullptr) {
        return;
    }

    xSemaphoreTake(_mutex, portMAX_DELAY);

    _configDirty = true;

    xSemaphoreGive(_mutex);
}

const char* AtCommandManager::modToString(classModulation mod) {
    switch (mod) {
        case classModulation::GFSK100: return "GFSK100";
        case classModulation::GFSK250: return "GFSK250";
        case classModulation::LORA_SF7: return "LORA_SF7";
        case classModulation::LORA_SF9: return "LORA_SF9";
        default: return "UNKNOWN";
    }
}

const char* AtCommandManager::linkToString(classLinkState state) {
    switch (state) {
        case classLinkState::SYNC: return "SYNC";
        case classLinkState::TRACKING: return "TRACKING";
        case classLinkState::LOST: return "LOST";
        case classLinkState::DEGRADED: return "DEGRADED";
        default: return "UNKNOWN";
    }
}

bool AtCommandManager::takePendingCommand(String& outCmd) {
    if (_mutex == nullptr) return false;
    bool hasCmd = false;
    xSemaphoreTake(_mutex, portMAX_DELAY);
    if (_cmdCount > 0) {
        outCmd = _cmdQueue[_cmdHead];
        _cmdHead = (_cmdHead + 1) % 8;
        _cmdCount--;
        hasCmd = true;
    }
    xSemaphoreGive(_mutex);
    return hasCmd;
}

ModemConfig AtCommandManager::getCurrentConfig() {
    ModemConfig copy{};
    if (_mutex != nullptr) {
        xSemaphoreTake(_mutex, portMAX_DELAY);
        copy = _cfg;
        xSemaphoreGive(_mutex);
    }
    return copy;
}
