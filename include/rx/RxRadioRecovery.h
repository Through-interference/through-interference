#pragma once

#include <functional>
#include <Arduino.h>

class RxRadioRecovery {
public:
    RxRadioRecovery(bool& transmitFlag, volatile bool& operationDone);

    using ReinitCallback = std::function<bool()>;
    void setReinitCallback(ReinitCallback cb) { reinitCb_ = cb; }
    bool initializeRadio();
    void scheduleFullRecovery(const char* reason);
    bool startReceiveWithRecovery();
    void update();

    unsigned long receiveStartFailures() const;
    unsigned long successfulRecoveries() const;
    unsigned long failedRecoveryAttempts() const;

    bool isRecovering() const;

private:
    bool& transmitFlag_;
    volatile bool& operationDone_;

    bool radioInitialized_ = false;
    bool receiveRetryPending_ = false;
    unsigned long receiveRetryAt_ = 0;
    int receiveRetryCount_ = 0;
    bool fullRecoveryPending_ = false;
    unsigned long fullRecoveryAt_ = 0;
    unsigned long fullRecoveryDelayMs_ = 1000UL;

    unsigned long receiveStartFailures_ = 0;
    unsigned long successfulRecoveries_ = 0;
    unsigned long failedRecoveryAttempts_ = 0;

    ReinitCallback reinitCb_ = nullptr;
    
    void serviceFullRecovery();
    void serviceReceiveRetry();
};
