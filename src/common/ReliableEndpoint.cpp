#include "ReliableEndpoint.h"

namespace canlink {
bool ReliableEndpoint::enqueue(const CanFrame& frame) {
    return pending() < 64 && outgoing_.push(frame);
}
Packet ReliableEndpoint::packet(PacketKind kind, bool includePayload, bool ready) {
    Packet p; p.kind = kind; p.session = session_; p.ready = ready;
    p.destinationSession = peerSession_;
    p.ackSession = receivedSession_; p.ackSequence = receivedSequence_;
    if (!includePayload || !peerSession_) return p;
    if (!activeCount_ && outgoing_.size()) {
        // A new session avoids sequence reuse even after a 32-bit wrap.
        if (!nextSequence_) { ++session_; if (!session_) session_ = 1; nextSequence_ = 1; }
        activeSequence_ = nextSequence_++;
        while (activeCount_ < BatchSize && outgoing_.pop(active_[activeCount_])) ++activeCount_;
    }
    p.session = session_; p.sequence = activeCount_ ? activeSequence_ : 0; p.count = activeCount_;
    for (size_t i = 0; i < activeCount_; ++i) p.frames[i] = active_[i];
    return p;
}
bool ReliableEndpoint::receive(const Packet& p, FrameSink& sink) {
    const bool discovery = p.kind == PacketKind::Poll && !p.destinationSession && !p.count;
    if (!p.session || (p.destinationSession != session_ && !discovery)) return false;
    if (peerSession_ && peerSession_ != p.session) {
        // An in-flight batch may already have executed before the peer rebooted.
        // Never redirect it to the new boot session or call it successfully delivered.
        uncertain_ += activeCount_; activeCount_ = 0; activeSequence_ = 0;
        receivedSession_ = 0; receivedSequence_ = 0; receivedCount_ = 0;
    }
    peerSession_ = p.session;
    if (activeCount_ && p.ackSession == session_ && p.ackSequence == activeSequence_) {
        activeCount_ = 0; activeSequence_ = 0;
    }
    if (!p.count) return true;
    if (p.count > BatchSize || !p.sequence || !p.session) return false;
    for (size_t i = 0; i < p.count; ++i) if (!validFrame(p.frames[i])) return false;
    if (p.session == receivedSession_ && p.sequence <= receivedSequence_) {
        if (p.sequence != receivedSequence_ || p.count != receivedCount_) return false;
        for (size_t i = 0; i < p.count; ++i) if (!equalFrame(p.frames[i], received_[i])) return false;
        return true;
    }
    if (!sink.accept(p.frames, p.count)) return false;
    receivedSession_ = p.session; receivedSequence_ = p.sequence; receivedCount_ = p.count;
    for (size_t i = 0; i < p.count; ++i) received_[i] = p.frames[i];
    return true;
}
}
