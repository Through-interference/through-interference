#pragma once
#include <Arduino.h>
#include "AtCommandManager.h"
#include "CanHardware.h"
#include "ReliableEndpoint.h"

class BridgeController : public canlink::FrameSink {
public:
    explicit BridgeController(bool airborne) : airborne_(airborne), at_(Serial, 255), endpoint_(1) {}
    void begin();
    void update();
    bool accept(const canlink::CanFrame* frames, size_t count) override;
private:
    bool airborne_;
    AtCommandManager at_;
    canlink::ReliableEndpoint endpoint_;
    CanHardware can_;
    canlink::FrameQueue<64> usbRx_;
    ModemConfig config_;
    bool cryptoReady_ = false;
    bool radioReady_ = false;
    bool transmitting_ = false;
    bool waiting_ = false;
    bool responsePending_ = false;
    bool usbOpen_ = false;
    bool connected_ = false;
    bool peerReady_ = false;
    unsigned long txAt_ = 0;
    unsigned long deadline_ = 0;
    unsigned long nextPollAt_ = 0;
    unsigned long replyAt_ = 0;
    unsigned long lastPeerAt_ = 0;
    unsigned long lastChannelAt_ = 0;
    unsigned long retryRadioAt_ = 0;
    unsigned long retryCanAt_ = 0;
    unsigned long usbLeaseAt_ = 0;
    unsigned long nextUsbAt_ = 0;
    uint32_t usbDeliveryId_ = 1;
    uint32_t usbDelivered_ = 0;
    uint32_t radioTx_ = 0;
    uint32_t radioRx_ = 0;
    uint32_t timeouts_ = 0;
    uint32_t badPackets_ = 0;
    uint32_t queueRejected_ = 0;
    uint64_t replyDestination_ = 0;
    uint32_t reportedUncertain_ = 0;

    static bool extension(void* context, const String& line, Stream& port);
    static bool canBaud(void* context, uint32_t bitrate);
    bool command(const String& line, Stream& port);
    bool configure(const ModemConfig& config);
    bool startReceive();
    void radioFailure(const char* stage, int state);
    void startCan();
    void send(canlink::PacketKind kind, bool includePayload);
    void receive();
    void serviceUsb();
    void recoverChannel();
    unsigned long receiveWindow() const;
};
