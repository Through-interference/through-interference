#pragma once
#include <stddef.h>
#include <stdint.h>

namespace canlink {
constexpr size_t BatchSize = 4;
constexpr size_t PacketCapacity = 96;

struct CanFrame {
    uint32_t id = 0;
    uint8_t dlc = 0;
    bool extended = false;
    bool remote = false;
    uint8_t data[8] = {};
};

bool validFrame(const CanFrame& frame);
bool equalFrame(const CanFrame& left, const CanFrame& right);
bool parseCanFrame(const char* line, CanFrame& frame);
bool formatCanFrame(const CanFrame& frame, char* output, size_t capacity);

enum class PacketKind : uint8_t { Poll = 1, Response = 2 };
struct Packet {
    PacketKind kind = PacketKind::Poll;
    uint64_t session = 0;
    uint32_t sequence = 0;
    uint64_t destinationSession = 0;
    uint64_t ackSession = 0;
    uint32_t ackSequence = 0;
    uint8_t count = 0;
    bool ready = false;
    CanFrame frames[BatchSize];
};

bool encodePacket(const Packet& packet, uint8_t* output, size_t capacity, size_t& length);
bool decodePacket(const uint8_t* input, size_t length, Packet& packet);

template <size_t Capacity> class FrameQueue {
public:
    size_t size() const { return count_; }
    size_t free() const { return Capacity - count_; }
    bool push(const CanFrame& frame) {
        if (!validFrame(frame) || count_ == Capacity) return false;
        frames_[(head_ + count_) % Capacity] = frame;
        ++count_; return true;
    }
    bool peek(CanFrame& frame) const {
        if (!count_) return false;
        frame = frames_[head_]; return true;
    }
    bool pop(CanFrame& frame) {
        if (!peek(frame)) return false;
        head_ = (head_ + 1) % Capacity; --count_; return true;
    }
    void clear() { head_ = count_ = 0; }
private:
    CanFrame frames_[Capacity];
    size_t head_ = 0;
    size_t count_ = 0;
};

class FrameSink {
public:
    virtual ~FrameSink() {}
    // Accept the whole batch or none of it. False provides backpressure.
    virtual bool accept(const CanFrame* frames, size_t count) = 0;
};
}
