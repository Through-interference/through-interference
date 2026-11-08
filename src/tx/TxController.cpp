#include "tx/TxController.h"

#include <RadioLib.h>
#include "crypto.h"
#include "protocol.h"
#include "radio_config.h"
#include "radio_hardware.h"
#include "RadioChannelManager.h"
#include "AtCommandManager.h"
#include "frequency_config.h"

namespace {
const unsigned long ACK_TIMEOUT_MS = 3000;
const int MAX_RETRIES = 3;
const int DEGRADED_AFTER_ACK_TIMEOUTS = 2;

const unsigned long DATA_TX_TIMEOUT_MS = 5000UL;

// Пины для UART2 (CP2102) - боевой канал данных для дрона
const int DRONE_UART_RX = 16;
const int DRONE_UART_TX = 17;
bool applyModemConfigToRadio(const ModemConfig& newCfg) {
    radio.standby();

    // 1. Проверяем допустимый диапазон частот.
    if (newCfg.primFreqMhz < 850.0f || newCfg.primFreqMhz > 930.0f ||
        newCfg.backFreqMhz < 850.0f || newCfg.backFreqMhz > 930.0f) {
        Serial.println("[TX][AT] Invalid frequency configuration");
        return false;
    }

    // 2. Обновляем частоты в менеджере каналов.
    if (!radioChannelManager.setPrimaryFrequency(newCfg.primFreqMhz)) {
        Serial.println("[TX][AT] Failed to set primary frequency");
        return false;
    }

    if (!radioChannelManager.setBackupFrequency(newCfg.backFreqMhz)) {
        Serial.println("[TX][AT] Failed to set backup frequency");
        return false;
    }

    const float currentFreq = radioChannelManager.currentFrequencyMHz();
    int16_t state = RADIOLIB_ERR_NONE;

    // 3. Применяем модуляцию.
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
            Serial.println("[TX][AT] Unsupported modulation");
            return false;
    }

    // 4. Проверяем результат настройки мощности.
    if (state == RADIOLIB_ERR_NONE) {
        state = radio.setOutputPower(newCfg.txPowerDbm);
    }

    // 5. Проверяем все операции RadioLib, которые возвращают код ошибки.
    if (state != RADIOLIB_ERR_NONE) {
        Serial.print("[TX][AT] Radio reconfiguration error: ");
        Serial.println(state);
        return false;
    }

    // 6. Настраиваем аппаратные пины и обработчик прерывания.
    radio.setRfSwitchPins(LORA_RXEN, LORA_TXEN);
    radio.setDio1Action(setFlag);

    // 7. Проверяем, есть ли новый AES-ключ.
    bool hasCustomKey = false;

    for (int i = 0; i < 16; i++) {
        if (newCfg.aesKey[i] != 0) {
            hasCustomKey = true;
            break;
        }
    }

    // 8. Если ключ задан, обновляем его.
    if (hasCustomKey) {
        if (!cryptoUpdateKey(newCfg.aesKey)) {
            Serial.println("[TX][AT] AES-128 key update FAILED");
            return false;
        }

        Serial.println("[TX][AT] AES-128 key updated");
    }

    Serial.println("[TX][AT] Radio configuration applied successfully");
    return true;
}
}

// 1. UART0 (Serial) — сервисный порт (255 = не перехватывать пин LORA_TXEN)
AtCommandManager atManager(Serial, 255);

void TxController::begin() {
    Serial.begin(115200);
    delay(1000);

    // Загружаем конфигурацию из NVS (если она есть, взводится флаг _configDirty)
    atManager.begin();

    // 2. Боевой порт (CP2102 / UART2) для команд на дрон
    Serial2.setRxBufferSize(1024);
    Serial2.begin(115200, SERIAL_8N1, DRONE_UART_RX, DRONE_UART_TX);
    

    if (!cryptoInit()) {
        Serial.println("AES-GCM initialization failed!");
        while (true) delay(1000);
    }

    if (!outbox_.begin()) {
        Serial.println("[TX] Cannot open NVS namespace for message IDs");
        while (true) delay(1000);
    }

    Serial.print("[TX] Node ID: ");
    Serial.println(TX_NODE_ID);

    Serial.print("[TX] Default destination ID: ");
    Serial.println(RX_NODE_ID);

    radio.setRfSwitchPins(LORA_RXEN, LORA_TXEN);
    radio.tcxoVoltage = 1.8;

    Serial.print("[SX1262] Initializing ... ");

    ConfigLoRa_t config;
    config.frequency = radioChannelManager.currentFrequencyMHz();

    const int state = radio.begin(config);

    if (state != RADIOLIB_ERR_NONE) {
        Serial.print("Radio initialization failed: ");
        Serial.println(state);
        while (true) delay(1000);
    }

    radio.setDio1Action(setFlag);

    lastHeartbeatAt_ = millis();

    // ВАЖНО: Применяем сохраненный профиль из NVS ДО создания/восстановления пакета в outbox_!
    ModemConfig bootCfg;
    bool hasValidKey = false;

    if (atManager.takePendingConfig(bootCfg)) {
        const bool configApplied = applyModemConfigToRadio(bootCfg);

        if (configApplied) {
            Serial.println("[TX][BOOT] Bind config applied successfully");
            currentModulation_ = static_cast<uint8_t>(bootCfg.modulation);
            hasAppliedOnce_ = true;

            for (int i = 0; i < 16; i++) {
                if (bootCfg.aesKey[i] != 0) {
                    hasValidKey = true;
                    break;
                }
            }
        } else {
            Serial.println("[TX][BOOT] Failed to apply bind config");
        }
    }
    Serial.println("Encrypted transmitter started");
    // БЛОКИРОВКА РАДИООБМЕНА, ЕЛИ КЛЮЧА НЕТ
    if (!hasValidKey) {
        Serial.println("[CRITICAL] Radio transmission BLOCKED. Run bind.py to configure encryption.");
        while (true) {
            atManager.processInput(); // Оставляем доступ только к AT-командам (включая bind.py)
            delay(50);
        }
    }
    ackCount_ = 0;

    // Принудительно очищаем старые зависшие пакеты
    if (outbox_.isActive()) {
        outbox_.acknowledge();
        Serial.println("[TX] Wiped old pending messages from previous session");
    }
    
    const int rxState = radio.startReceive();
    if (rxState != RADIOLIB_ERR_NONE) {
        Serial.print("[TX] Initial startReceive failed: ");
        Serial.println(rxState);
    }
    Serial.println("[TX] Ready for commands");
    // if (!outbox_.restore()) {
    //     if (outbox_.sequence() <= TOTAL_MESSAGES) {
    //         //startNextData();
    //     } else {
    //         Serial.println(
    //             "[TX] No pending outbox and test sequence already complete"
    //         );
    //     }
    // } else {
    //     retryCount_ = 0;
    //     nextDataPending_ = false;
    //     sendCurrentData();
    // }

    // ackCount_ = (outbox_.sequence() > TOTAL_MESSAGES)
    //     ? TOTAL_MESSAGES
    //     : static_cast<int>(outbox_.sequence() - 1);
}


void TxController::update() {
    // 1. Обрабатываем команды с UART0 (Type-C)
    atManager.processInput();

    // 2. Применяем конфигурацию PHY/MAC/KEY немедленно между передачами
    if (!transmitFlag_ && !operationDone && !pendingOtaModChange_) {
        ModemConfig newCfg;

        if (atManager.takePendingConfig(newCfg)) {
            // Обязательно сбрасываем поиск канала, как мы делали в фиксе дедлока!
            if (radioChannelManager.isSearching()) {
                radioChannelManager.stopSearch();
            }

            const uint8_t newMod = static_cast<uint8_t>(newCfg.modulation);

            // Если изменилась именно МОДУЛЯЦИЯ — сначала шлем приказ ресиверу!
            if (hasAppliedOnce_ && newMod != currentModulation_) {
                String modName = "LORA_SF9";
                if (newCfg.modulation == classModulation::GFSK100) modName = "GFSK100";
                else if (newCfg.modulation == classModulation::GFSK250) modName = "GFSK250";
                else if (newCfg.modulation == classModulation::LORA_SF7) modName = "LORA_SF7";

                String otaCmd = "SYS:MOD:" + modName;
                Serial.print("[TX][OTA] Negotiating modulation change with RX: ");
                Serial.println(otaCmd);

                // ВАЖНО: Если висит зависшая боевая команда, отменяем её принудительно (иначе outbox_.create вернет false)
                if (outbox_.isActive()) {
                    outbox_.acknowledge();
                    Serial.println("[TX][OTA] Dropped stuck drone command to prioritize system update");
                }

                if (outbox_.create(otaCmd)) {
                    pendingOtaModChange_ = true;
                    pendingOtaCfg_ = newCfg;
                    retryCount_ = 0;
                    consecutiveAckTimeouts_ = 0;
                    sendCurrentData();
                } else {
                    atManager.retryPendingConfig();
                }
            } else {
                // Обычное применение (частота, мощность, ключ) применяем немедленно!
                if (applyModemConfigToRadio(newCfg)) {
                    currentModulation_ = newMod;
                    hasAppliedOnce_ = true;
                    Serial.println("[TX][AT] Applied new radio config");
                    
                    // Если в очереди висит неподтвержденная команда — сразу переотправляем её на новых настройках!
                    if (outbox_.isActive()) {
                        retryCount_ = 0;
                        consecutiveAckTimeouts_ = 0;
                        sendCurrentData();
                    } else {
                        const int rxState = radio.startReceive();
                        if (rxState != RADIOLIB_ERR_NONE) {
                            Serial.print("[TX][AT] Failed to start RX: ");
                            Serial.println(rxState);
                        }
                    }
                } else {
                    Serial.println("[TX][AT] Failed to apply radio config");
                    atManager.retryPendingConfig();
                }
            }
        }
    }
    // 3. Читаем боевые команды с правильным приоритетом
    if (!outbox_.isActive() && !transmitFlag_) {
        String droneCmd;
        
        // Высший приоритет: команды от оператора из Python-панели (USB/AT)
        if (atManager.takePendingCommand(droneCmd)) {
            // Команда забрана из кольцевого буфера
        } 
        // Низший приоритет: телеметрия от полетного контроллера (UART2)
        else if (Serial2.available()) {
            droneCmd = Serial2.readStringUntil('\n');
            droneCmd.trim();
        }

        if (droneCmd.length() > 0) {
            Serial.print("[TX][CMD] Drone command: ");
            Serial.println(droneCmd);

            if (outbox_.create(droneCmd)) {
                retryCount_ = 0;
                consecutiveAckTimeouts_ = 0;
                sendCurrentData();
            }
        }
    }

    // 4. Обновление телеметрии для RSSI / SNR / STAT
    ModemTelemetry telem;
    telem.rssiDbm = radio.getRSSI();
    telem.snrDb = radio.getSNR();
    telem.txPackets = ackCount_;
    telem.rxPackets = ackCount_;
    telem.linkState = (connectionState_ == CONNECTED) ? classLinkState::TRACKING : classLinkState::LOST;
    atManager.updateTelemetry(telem);

    handleRadioEvent();
    handleTxWatchdog();

    if (radioChannelManager.isSearching()) {
        updateChannelSearch();
    } else {
        handleAckTimeout();
    }

    // Heartbeat has the lowest priority.
    if (!outbox_.isActive() &&
        !transmitFlag_ &&
        !operationDone &&
        !radioChannelManager.isSearching() &&
        millis() - lastHeartbeatAt_ >=
            LINK_HEARTBEAT_INTERVAL_MS) {

        sendHeartbeat();
    }

    yield();
}

void TxController::sendCurrentData() {
    radio.standby();
    operationDone = false;
    heartbeatTx_ = false;

    Serial.print("[TX] Sending DATA messageId=");
    Serial.println(outbox_.messageId());

    String frame = outbox_.encryptedFrame();

    transmissionState_ = radio.startTransmit(frame);

    if (transmissionState_ == RADIOLIB_ERR_NONE) {
        transmitFlag_ = true;
        dataTxStartedAt_ = millis();
        ackTimer_ = millis();
    } else {
        Serial.print("[TX] startTransmit failed: ");
        Serial.println(transmissionState_);

        transmitFlag_ = false;
        ackTimer_ = millis() - ACK_TIMEOUT_MS - 1;
    }
}


void TxController::handleRadioEvent() {
    if (!operationDone) return;

    operationDone = false;

    if (transmitFlag_) {
        transmissionState_ = radio.finishTransmit();

        if (!heartbeatTx_) {
            if (transmissionState_ == RADIOLIB_ERR_NONE) {
                Serial.println("[TX] Transmission finished");
            } else {
                Serial.print("[TX] Transmission error: ");
                Serial.println(transmissionState_);
            }
        }

        transmitFlag_ = false;
        heartbeatTx_ = false;

        const int state = radio.startReceive();

        if (state != RADIOLIB_ERR_NONE) {
            Serial.print("[TX] startReceive failed: ");
            Serial.println(state);

            if (radioChannelManager.isSearching()) {
                nextSearchTxAt_ = millis() + 200;
            } else {
                ackTimer_ = millis() - ACK_TIMEOUT_MS - 1;
            }
        } else {
            ackTimer_ = millis();
            // даем приемнику TX гарантированное время дождаться ACK после окончания TX!
            if (radioChannelManager.isSearching()) {
                nextSearchTxAt_ = millis() + TX_SEARCH_REPEAT_MS; 
            }
        }

        return;
    }

    handleReceivedAck();
}


void TxController::handleReceivedAck() {
    String encryptedFrame;

    const int state = radio.readData(encryptedFrame);

    if (state != RADIOLIB_ERR_NONE) {
        Serial.print("[TX] readData failed: ");
        Serial.println(state);

        radio.startReceive();
        return;
    }

    Serial.print("[TX] Received encrypted frame: ");
    Serial.println(encryptedFrame);

    Serial.print("[TX] RSSI: ");
    Serial.print(radio.getRSSI());
    Serial.println(" dBm");

    Serial.print("[TX] SNR: ");
    Serial.print(radio.getSNR());
    Serial.println(" dB");

    String plaintext;

    uint32_t senderId = 0;
    uint64_t cryptoCounter = 0;

    const CryptoPacketStatus cryptoStatus = decryptPacketEx(
        encryptedFrame,
        plaintext,
        senderId,
        cryptoCounter
    );

    const char* cryptoStatusName = "UNKNOWN";

    switch (cryptoStatus) {
        case CRYPTO_PACKET_ERROR:
            cryptoStatusName = "ERROR";
            break;

        case CRYPTO_PACKET_NEW:
            cryptoStatusName = "NEW";
            break;

        case CRYPTO_PACKET_DUPLICATE:
            cryptoStatusName = "DUPLICATE";
            break;

        case CRYPTO_PACKET_REPLAY:
            cryptoStatusName = "REPLAY";
            break;
    }

    Serial.printf(
        "[TX][ACK-CRYPTO] status=%s sender=0x%08lX "
        "counter=0x%016llX expectedPeer=0x%08lX\n",
        cryptoStatusName,
        (unsigned long)senderId,
        (unsigned long long)cryptoCounter,
        (unsigned long)CRYPTO_PEER_ID
    );

    if (cryptoStatus != CRYPTO_PACKET_NEW &&
        cryptoStatus != CRYPTO_PACKET_DUPLICATE) {

        Serial.println(
            "[TX] ACK failed authentication/replay check; ignored"
        );

        radio.startReceive();
        return;
    }

    RadioMessage received;

    const bool matchingAck =
        decodeRadioMessage(plaintext, received) &&
        received.type == RADIO_MSG_ACK &&
        received.senderId == RX_NODE_ID &&
        received.destinationId == TX_NODE_ID &&
        received.messageId == outbox_.messageId() &&
        received.payload == "OK" &&
        senderId == static_cast<uint32_t>(CRYPTO_PEER_ID);

    if (!matchingAck) {
        Serial.print(
            "[TX] ACK does not match current transaction; ignored. "
            "Current messageId="
        );

        Serial.println(outbox_.messageId());

        radio.startReceive();
        return;
    }

    Serial.print("[TX] ACK matched messageId=");
    Serial.println(received.messageId);

    if (!outbox_.acknowledge()) {
        Serial.println(
            "[TX] ACK was valid, but outbox commit failed; "
            "transaction remains pending"
        );

        ackTimer_ = millis() - ACK_TIMEOUT_MS - 1;

        radio.startReceive();
        return;
    }
    // Если это был ACK на команду смены модуляции SYS:MOD:...
    if (pendingOtaModChange_) {
        pendingOtaModChange_ = false;
        Serial.println("[TX][OTA] RX confirmed modulation change! Switching TX radio now...");
        if (applyModemConfigToRadio(pendingOtaCfg_)) {
            currentModulation_ = static_cast<uint8_t>(pendingOtaCfg_.modulation);
            atManager.saveToNvs(); // Автоматически сохраняем в NVS передатчика
        }
        radio.startReceive();
    }

    const bool recoveredBySearch = radioChannelManager.isSearching();

    if (recoveredBySearch) {

        radioChannelManager.stopSearch();

        switchAfterAck_ = false;

        Serial.print("[TX][CHANNEL] Connection restored on channel ");
        Serial.println(radioChannelManager.currentChannel());

    } else {

        if (switchAfterAck_) {
            Serial.println(
                "[TX][CHANNEL] Keeping current channel after ACK"
            );
        }

        // Не переключаем частоту самостоятельно.
        // RX остаётся на канале, на котором отправил ACK.
        switchAfterAck_ = false;
    }

    lastHeartbeatAt_ = millis();

    ackCount_++;

    retryCount_ = 0;
    consecutiveAckTimeouts_ = 0;

    setConnectionState(CONNECTED);
}


void TxController::handleTxWatchdog() {
    if (!transmitFlag_ ||
        (unsigned long)(millis() - dataTxStartedAt_) < DATA_TX_TIMEOUT_MS) {
        return;
    }

    Serial.println(
        "[TX] DATA TX completion timeout; resetting radio state, "
        "outbox retained"
    );

    radio.standby();

    if (radioChannelManager.isSearching()) {
        nextSearchTxAt_ = millis();
    } else {
        ackTimer_ = millis() - ACK_TIMEOUT_MS - 1;
    }

    transmitFlag_ = false;
    operationDone = false;
}


void TxController::handleAckTimeout() {
    if (radioChannelManager.isSearching() ||
        transmitFlag_ ||
        !outbox_.isActive() ||
        millis() - ackTimer_ <= ACK_TIMEOUT_MS) {
        return;
    }

    consecutiveAckTimeouts_++;

    Serial.print("[TX] ACK timeout; consecutive timeouts: ");
    Serial.println(consecutiveAckTimeouts_);

    if (consecutiveAckTimeouts_ >= DEGRADED_AFTER_ACK_TIMEOUTS) {
        if (connectionState_ == CONNECTED) {
            setConnectionState(DEGRADED);
        }

        switchAfterAck_ = true;
    }

    if (retryCount_ < MAX_RETRIES) {
        retryCount_++;
        totalNormalRetries_++;

        Serial.print("[TX] Normal retry ");
        Serial.print(retryCount_);
        Serial.print("/");
        Serial.println(MAX_RETRIES);

        sendCurrentData();
        return;
    }

    startChannelSearch();
}

void TxController::setConnectionState(ConnectionState nextState) {
    if (connectionState_ == nextState) return;

    const unsigned long now = millis();

    if (connectionState_ == RECOVERY) {
        lastRecoveryDurationMs_ = now - recoveryStartedAt_;
        totalRecoveryTimeMs_ += lastRecoveryDurationMs_;

        if (nextState == CONNECTED) {
            successfulRecoveries_++;

            Serial.print("[TX] Recovery completed in ");
            Serial.print(lastRecoveryDurationMs_);
            Serial.println(" ms");
        }
    }

    connectionState_ = nextState;

    Serial.print("[TX] Connection state: ");

    if (nextState == CONNECTED) {
        Serial.println("CONNECTED");

    } else if (nextState == DEGRADED) {
        Serial.println("DEGRADED");

    } else {
        recoveryStartedAt_ = now;
        recoveryEpisodes_++;

        Serial.println("RECOVERY");
    }
}

void TxController::startChannelSearch() {
    if (radioChannelManager.isSearching()) return;

    Serial.println("[TX][CHANNEL] Starting channel search");

    if (!radioChannelManager.startSearch(TX_CHANNEL_DWELL_MS)) {
        Serial.println("[TX][CHANNEL] Cannot start channel search");

        ackTimer_ = millis();

        return;
    }

    setConnectionState(RECOVERY);

    totalRecoveryAttempts_++;

    nextSearchTxAt_ = millis();
}


void TxController::updateChannelSearch() {
    if (!radioChannelManager.isSearching() || transmitFlag_) {
        return;
    }

    const uint8_t previousChannel =
        radioChannelManager.currentChannel();

    if (radioChannelManager.updateSearch()) {
        totalRecoveryAttempts_++;

        if (radioChannelManager.currentChannel() == RADIO_PRIMARY_CHANNEL &&
            previousChannel != RADIO_PRIMARY_CHANNEL) {

            retryCyclesExhausted_++;
        }

        Serial.print("[TX][CHANNEL] Searching on channel ");
        Serial.println(radioChannelManager.currentChannel());
    }

    if ((long)(millis() - nextSearchTxAt_) >= 0) {
        nextSearchTxAt_ = millis() + TX_SEARCH_REPEAT_MS;

        Serial.println("[TX][CHANNEL] Repeating pending DATA");

        sendCurrentData();
    }
}

void TxController::sendHeartbeat() {

    lastHeartbeatAt_ = millis();

    RadioMessage heartbeat;

    heartbeat.type = RADIO_MSG_HEARTBEAT;
    heartbeat.senderId = TX_NODE_ID;
    heartbeat.destinationId = RX_NODE_ID;
    heartbeat.messageId = 0;
    heartbeat.payload = "PING";

    String plaintext;
    String encryptedFrame;

    if (!encodeRadioMessage(heartbeat, plaintext) ||
        !encryptPacket(plaintext, encryptedFrame)) {
        return;
    }

    radio.standby();
    operationDone = false;

    heartbeatTx_ = true;

    transmissionState_ = radio.startTransmit(encryptedFrame);

    if (transmissionState_ == RADIOLIB_ERR_NONE) {

        transmitFlag_ = true;
        dataTxStartedAt_ = millis();

    } else {

        heartbeatTx_ = false;
        radio.startReceive();
    }
}