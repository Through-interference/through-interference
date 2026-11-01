#pragma once

#include <Arduino.h>
#include "tx/TxOutbox.h"

class TxController {
public:
    void begin();
    void update();

private:
    enum ConnectionState {
        CONNECTED,
        DEGRADED,
        RECOVERY
    };

    TxOutbox outbox_;

    bool transmitFlag_ = false;
    int transmissionState_ = 0;

    ConnectionState connectionState_ = CONNECTED;
    unsigned long recoveryStartedAt_ = 0;
    unsigned long totalRecoveryTimeMs_ = 0;
    unsigned long lastRecoveryDurationMs_ = 0;
    int recoveryEpisodes_ = 0;
    int successfulRecoveries_ = 0;

    int ackCount_ = 0;
    uint32_t retryCyclesExhausted_ = 0;
    int retryCount_ = 0;
    int consecutiveAckTimeouts_ = 0;
    int totalNormalRetries_ = 0;
    int totalRecoveryAttempts_ = 0;
    unsigned long ackTimer_ = 0;
    unsigned long dataTxStartedAt_ = 0;

    bool nextDataPending_ = false;
    unsigned long nextDataAt_ = 0;

    bool switchAfterAck_ = false;
    unsigned long nextSearchTxAt_ = 0;

    void startNextData();
    void sendCurrentData();
    void handleRadioEvent();
    void handleReceivedAck();
    void handleTxWatchdog();
    void handleAckTimeout();
    void handleScheduledData();
    void setConnectionState(ConnectionState nextState);
    void printSummary();
    void startChannelSearch();
    void updateChannelSearch();
};
