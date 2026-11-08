#pragma once
#include "CanProtocol.h"

namespace canlink {
class ReliableEndpoint {
public:
    explicit ReliableEndpoint(uint64_t session) : session_(session ? session : 1) {}
    bool enqueue(const CanFrame& frame);
    size_t pending() const { return outgoing_.size() + activeCount_; }
    size_t free() const { return 64 - pending(); }
    Packet packet(PacketKind kind, bool includePayload, bool ready);
    bool receive(const Packet& packet, FrameSink& sink);
    uint64_t peerSession() const { return peerSession_; }
    uint32_t uncertain() const { return uncertain_; }
private:
    uint64_t session_;
    uint64_t peerSession_ = 0;
    uint32_t uncertain_ = 0;
    uint32_t nextSequence_ = 1;
    uint32_t activeSequence_ = 0;
    uint8_t activeCount_ = 0;
    CanFrame active_[BatchSize];
    FrameQueue<64> outgoing_;
    uint64_t receivedSession_ = 0;
    uint32_t receivedSequence_ = 0;
    uint8_t receivedCount_ = 0;
    CanFrame received_[BatchSize];
};
}
