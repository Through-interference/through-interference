#include "rx/RxRadioRecovery.h"

#include <RadioLib.h>
#include "radio_config.h"
#include "radio_hardware.h"
#include "RadioChannelManager.h"

namespace {
const unsigned long RX_RETRY_DELAY_MS = 250UL;
const int MAX_START_RECEIVE_RETRIES = 3;
const unsigned long RADIO_RECOVERY_INITIAL_MS = 1000UL;
const unsigned long RADIO_RECOVERY_MAX_MS = 8000UL;
}

RxRadioRecovery::RxRadioRecovery(bool& transmitFlag, volatile bool& operationDone)
    : transmitFlag_(transmitFlag), operationDone_(operationDone) {
}

bool RxRadioRecovery::initializeRadio() {
    // Сначала пробуем восстановить радио с полным боевым конфигом (SF9, мощность, AES)
    if (reinitCb_ && reinitCb_()) {
        radioInitialized_ = true;
        return true;
    }

    // Если рабочего конфига еще нет (самый первый старт до загрузки NVS) - базовый фоллбэк
    radio.setRfSwitchPins(LORA_RXEN, LORA_TXEN);
    radio.tcxoVoltage = 1.8;

    ConfigLoRa_t config;
    config.frequency = radioChannelManager.currentFrequencyMHz();
    const int state = radio.begin(config);

    if (state != RADIOLIB_ERR_NONE) {
        radioInitialized_ = false;
        Serial.print("[RX] Radio initialization failed: ");
        Serial.println(state);
        return false;
    }

    radio.setDio1Action(setFlag);
    radioInitialized_ = true;
    return true;
}

void RxRadioRecovery::scheduleFullRecovery(const char* reason) {
    if (fullRecoveryPending_) return;

    Serial.print("[RX] Full radio recovery scheduled: ");
    Serial.println(reason);
    fullRecoveryPending_ = true;
    fullRecoveryAt_ = millis() + fullRecoveryDelayMs_;
    receiveRetryPending_ = false;
    receiveRetryCount_ = 0;
}

bool RxRadioRecovery::startReceiveWithRecovery() {
    if (fullRecoveryPending_) return false;

    if (!radioInitialized_) {
        scheduleFullRecovery("radio is not initialized");
        return false;
    }

    const int state = radio.startReceive();
    if (state == RADIOLIB_ERR_NONE) {
        receiveRetryPending_ = false;
        receiveRetryCount_ = 0;
        return true;
    }

    receiveStartFailures_++;
    Serial.print("[RX] startReceive failed: ");
    Serial.println(state);
    receiveRetryPending_ = true;
    receiveRetryAt_ = millis() + RX_RETRY_DELAY_MS;
    return false;
}

void RxRadioRecovery::update() {
    if (fullRecoveryPending_) {
        serviceFullRecovery();
        return;
    }
    serviceReceiveRetry();
}

void RxRadioRecovery::serviceFullRecovery() {
    if ((long)(millis() - fullRecoveryAt_) < 0) return;

    Serial.println("[RX] Attempting radio reinitialization...");
    transmitFlag_ = false;
    operationDone_ = false;

    if (initializeRadio()) {
        const int state = radio.startReceive();
        if (state == RADIOLIB_ERR_NONE) {
            fullRecoveryPending_ = false;
            fullRecoveryDelayMs_ = RADIO_RECOVERY_INITIAL_MS;
            receiveRetryPending_ = false;
            receiveRetryCount_ = 0;
            successfulRecoveries_++;
            Serial.println("[RX] Radio recovered automatically; listening again");
            return;
        }
        Serial.print("[RX] startReceive after reinitialization failed: ");
        Serial.println(state);
    }

    failedRecoveryAttempts_++;
    fullRecoveryAt_ = millis() + fullRecoveryDelayMs_;
    Serial.print("[RX] Will retry full recovery in ");
    Serial.print(fullRecoveryDelayMs_);
    Serial.println(" ms");
    fullRecoveryDelayMs_ *= 2;
    if (fullRecoveryDelayMs_ > RADIO_RECOVERY_MAX_MS) {
        fullRecoveryDelayMs_ = RADIO_RECOVERY_MAX_MS;
    }
}

void RxRadioRecovery::serviceReceiveRetry() {
    if (!receiveRetryPending_ ||
        (long)(millis() - receiveRetryAt_) < 0) return;

    if (receiveRetryCount_ >= MAX_START_RECEIVE_RETRIES) {
        scheduleFullRecovery("repeated startReceive failures");
        return;
    }

    receiveRetryCount_++;
    Serial.print("[RX] startReceive retry ");
    Serial.print(receiveRetryCount_);
    Serial.print("/");
    Serial.println(MAX_START_RECEIVE_RETRIES);

    const int state = radio.startReceive();
    if (state == RADIOLIB_ERR_NONE) {
        receiveRetryPending_ = false;
        receiveRetryCount_ = 0;
        Serial.println("[RX] Receive mode restored");
    } else {
        receiveStartFailures_++;
        Serial.print("[RX] Receive retry failed: ");
        Serial.println(state);
        receiveRetryAt_ = millis() + RX_RETRY_DELAY_MS;
    }
}

unsigned long RxRadioRecovery::receiveStartFailures() const { return receiveStartFailures_; }
unsigned long RxRadioRecovery::successfulRecoveries() const { return successfulRecoveries_; }
unsigned long RxRadioRecovery::failedRecoveryAttempts() const { return failedRecoveryAttempts_; }
bool RxRadioRecovery::isRecovering() const { return fullRecoveryPending_ || receiveRetryPending_; }