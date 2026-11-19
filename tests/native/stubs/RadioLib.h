#pragma once
#include "Arduino.h"
#define RADIOLIB_ERR_NONE 0
#define RADIOLIB_ERR_INVALID_CURRENT_LIMIT (-17)
#define RADIOLIB_SX126X_SYNC_WORD_PRIVATE 0x12
class SX1262 {
public:
    int beginCalls = 0;
    int failedReceiveCalls = 0;
    int receiveCalls = 0;
    bool fault = false;
    bool receiving = false;
    int transmitState = 0;
    int readState = -2;
    bool frequencyFault = false;
    int beginState = 0;
    int currentLimitState = 0;
    int currentLimitCalls = 0;
    bool currentLimitWriteLost = false;
    int chipPower = 0;
    float currentLimit = 60;
    float currentLimitAtReceive = 0;
    int beginFSK(float, float, float, float, int power, int, float) {
        ++beginCalls; fault = false; receiving = false;
        chipPower = power; currentLimit = 60; return beginState;
    }
    int begin(float, float, int, int, int, int power, int, float) {
        ++beginCalls; fault = false; receiving = false;
        chipPower = power; currentLimit = 60; return beginState;
    }
    int setCurrentLimit(float value) {
        ++currentLimitCalls;
        if (currentLimitState == 0 && !currentLimitWriteLost) currentLimit = value;
        return currentLimitState;
    }
    float getCurrentLimit() const { return currentLimit; }
    void setRfSwitchPins(int, int) {}
    void setDio1Action(void (*)()) {}
    int standby() { receiving = false; return 0; }
    int startReceive() {
        ++receiveCalls;
        currentLimitAtReceive = currentLimit;
        if (fault) { ++failedReceiveCalls; receiving = false; return -2; }
        receiving = true; return 0;
    }
    int startTransmit(const String&) { receiving = false; return transmitState; }
    int finishTransmit() { return 0; }
    int readData(String&) { return readState; }
    int setFrequency(float) { receiving = false; return frequencyFault ? -2 : 0; }
    unsigned long getTimeOnAir(int) const { return 10000; }
    int getRSSI() const { return -50; }
    float getSNR() const { return 10; }
};
