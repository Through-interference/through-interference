#pragma once
#include "CanProtocol.h"
#include "ReliableEndpoint.h"
#include <driver/twai.h>

class CanHardware : public canlink::FrameSink {
public:
    bool begin(uint32_t bitrate);
    bool changeBitrate(uint32_t bitrate);
    bool accept(const canlink::CanFrame* frames, size_t count) override;
    void update(canlink::ReliableEndpoint& endpoint);
    size_t pending() const { return tx_.size(); }
    uint32_t received = 0;
    uint32_t transmitted = 0;
    uint32_t failures = 0;
    uint32_t dropped = 0;
    uint32_t busOffs = 0;
    bool ready() const { return installed_; }
private:
    canlink::FrameQueue<64> tx_;
    bool installed_ = false;
    bool awaitingTx_ = false;
    bool recovering_ = false;
    uint32_t bitrate_ = 0;
    unsigned long txAt_ = 0;
    unsigned long nextTxAt_ = 0;
};
