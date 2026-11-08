#pragma once

#include <Arduino.h>
#include "crypto.h"
#include "rx/RxMessageStore.h"
#include "rx/RxRadioRecovery.h"

class RxController {
public:
    RxController();
    void begin();
    void update();

private:
    bool transmitFlag_ = false;
    int transmissionState_ = 0;
    unsigned long ackTxStartedAt_ = 0;

    RxMessageStore messageStore_;
    RxRadioRecovery radioRecovery_;

    unsigned long lastStatsAt_ = 0;

    unsigned long receivedDataFrames_ = 0;
    unsigned long newDataMessages_ = 0;
    unsigned long duplicateDataFrames_ = 0;
    unsigned long ackTransmissionsStarted_ = 0;
    unsigned long ackTransmissionsCompleted_ = 0;
    unsigned long ackTransmissionFailures_ = 0;
    unsigned long decryptFailures_ = 0;

    bool scanAfterAck_ = false;

    uint8_t duplicateStreak_ = 0;

    unsigned long lastDataAt_ = 0;

    uint8_t testAckDrops_ = 0;
    bool pendingModSwitchAfterAck_ = false;
    String pendingModName_ = "";

    void startChannelSearch();
    void updateChannelSearch();

    void handleRadioEvent();
    void handleReceivedData();
    void processApplicationMessage(
        CryptoPacketStatus status,
        const String& plaintext,
        uint32_t cryptoSenderId,
        uint64_t cryptoCounter
    );
    void startAck(uint32_t messageId);
    void printPeriodicStats();
};
