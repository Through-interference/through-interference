#pragma once
#include <Arduino.h>
#include <Preferences.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

enum classModulation : uint8_t {
    GFSK100 = 0,
    GFSK250 = 1,
    LORA_SF7 = 2,
    LORA_SF9 = 3
};

enum classAntMode : uint8_t {
    ANT_1 = 1,
    ANT_2 = 2,
    ANT_AUTO = 0
};

enum classLinkState : uint8_t {
    SYNC = 0,
    TRACKING = 1,
    LOST = 2,
    DEGRADED = 3
};

// Структура конфигурации, сохраняемая во флеш (NVS) по AT+SAVE
struct ModemConfig {
    float primFreqMhz = 868.0f;
    float backFreqMhz = 869.1f;
    int8_t txPowerDbm = 20;
    classModulation modulation = classModulation::GFSK250;
    uint8_t tdmaSlot = 0;       // 0 = НСУ, 1..3 = БВС
    bool fhssEnabled = true;
    uint8_t aesKey[16] = {0};   // 128-битный ключ для crypto.h
    classAntMode antMode = classAntMode::ANT_AUTO;
    uint32_t canBaud = 500000;
};

// Структура телеметрии (обновляется радиоконтроллером, читается AT-командами)
struct ModemTelemetry {
    int16_t rssiDbm = -120;
    float snrDb = 0.0f;
    uint32_t txPackets = 0;
    uint32_t rxPackets = 0;
    float dutyCyclePercent = 0.0f;
    classLinkState linkState = classLinkState::LOST;
};

class AtCommandManager {
public:
    AtCommandManager(Stream& atPort, uint8_t rfSwitchPin);

    void begin();
    void processInput(); // Вызывать в задаче сервисного портаString _pendingDroneCmd;

    // Потокобезопасное получение настроек для RadioChannelManager и Tx/RxController
    bool takePendingConfig(ModemConfig& outCfg);
    bool takePendingCommand(String& outCmd);
    void updateTelemetry(const ModemTelemetry& newTelem);
    void retryPendingConfig();
    bool saveToNvs();
    ModemConfig getCurrentConfig();
    typedef bool (*ExtensionHandler)(void*, const String&, Stream&);
    typedef bool (*CanBaudHandler)(void*, uint32_t);
    void setExtensionHandler(ExtensionHandler handler, void* context) { _extension = handler; _context = context; }
    void setCanBaudHandler(CanBaudHandler handler) { _canBaud = handler; }

private:
    Stream& _port;
    uint8_t _rfSwitchPin;
    Preferences _prefs;
    SemaphoreHandle_t _mutex;

    String _cmdQueue[8];
    uint8_t _cmdCount = 0;
    uint8_t _cmdHead = 0;
    uint8_t _cmdTail = 0;

    ModemConfig _cfg;
    ModemTelemetry _telem;
    volatile bool _configDirty = false;
    String _rxLine;
    bool _lineOverflow = false;
    ExtensionHandler _extension = nullptr;
    CanBaudHandler _canBaud = nullptr;
    void* _context = nullptr;

    void handleLine(String line);
    void loadFromNvs();
    void restoreDefaults();
    void applyAntHardware(classAntMode mode);
    bool applyCanBaud(uint32_t baud);
    static bool parseHexKey(const String& hexStr, uint8_t* outKey);
    static const char* modToString(classModulation mod);
    static const char* linkToString(classLinkState state);
};
