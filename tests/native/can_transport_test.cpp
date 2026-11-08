#include "CanProtocol.h"
#include "ReliableEndpoint.h"
#include <assert.h>
#include <string.h>
#include <stdio.h>

using namespace canlink;

struct Sink : FrameSink {
    FrameQueue<256> frames;
    bool blocked = false;
    bool accept(const CanFrame* input, size_t count) override {
        if (blocked || frames.free() < count) return false;
        for (size_t i = 0; i < count; ++i) assert(frames.push(input[i]));
        return true;
    }
};

static CanFrame sample(uint32_t id = 0x123) {
    CanFrame f;
    f.id = id; f.dlc = 8;
    const uint8_t bytes[8] = {0, 0xff, 0xfe, 0xfd, 0x0a, 0x0d, 0, 0x80};
    memcpy(f.data, bytes, 8);
    return f;
}

static void codecs() {
    char line[32]; CanFrame decoded;
    CanFrame f = sample();
    assert(formatCanFrame(f, line, sizeof(line)));
    assert(strcmp(line, "t123800FFFEFD0A0D0080") == 0);
    assert(parseCanFrame(line, decoded)); assert(equalFrame(f, decoded));
    f.id = 0x1abcde01; f.extended = true;
    assert(formatCanFrame(f, line, sizeof(line)));
    assert(strcmp(line, "T1ABCDE01800FFFEFD0A0D0080") == 0);
    assert(parseCanFrame(line, decoded)); assert(equalFrame(f, decoded));
    f.remote = true; f.dlc = 7;
    assert(formatCanFrame(f, line, sizeof(line)));
    assert(strcmp(line, "R1ABCDE017") == 0);
    assert(parseCanFrame(line, decoded)); assert(equalFrame(f, decoded));
    assert(parseCanFrame("t0000", decoded)); assert(decoded.dlc == 0);
    const char* bad[] = {"", "t8000", "T200000000", "t1239", "t1231", "t1231GG", "t12310000", "r1231AA", "t1230junk"};
    for (const char* s : bad) assert(!parseCanFrame(s, decoded));

    Packet p; p.kind = PacketKind::Poll; p.session = 0x12345678; p.sequence = 7;
    p.destinationSession = 99; p.ackSession = 9; p.ackSequence = 2; p.ready = true; p.count = 4;
    for (size_t i = 0; i < 4; ++i) p.frames[i] = sample(0x120 + i);
    uint8_t bytes[96]; size_t size = 0;
    assert(encodePacket(p, bytes, sizeof(bytes), size)); assert(size == 92);
    assert(bytes[0] == 2 && bytes[1] == 1);
    assert(bytes[2] == 0x78 && bytes[5] == 0x12);
    Packet q; assert(decodePacket(bytes, size, q));
    assert(q.session == p.session && q.sequence == 7 && q.ackSequence == 2);
    assert(q.count == 4 && equalFrame(q.frames[3], p.frames[3]));
    assert(!decodePacket(bytes, size - 1, q));
    bytes[0] = 99; assert(!decodePacket(bytes, size, q)); bytes[0] = 2;
    bytes[34] = 5; assert(!decodePacket(bytes, size, q)); bytes[34] = 4;
    bytes[35] = 0x80; assert(!decodePacket(bytes, size, q));
    p.sequence = 0; assert(!encodePacket(p, bytes, sizeof(bytes), size));
}

static void queues() {
    FrameQueue<2> queue;
    assert(queue.push(sample(1)) && queue.push(sample(2)));
    assert(!queue.push(sample(3)));
    CanFrame f; assert(queue.pop(f) && f.id == 1);
    assert(queue.push(sample(3)));
    assert(queue.pop(f) && f.id == 2); assert(queue.pop(f) && f.id == 3);
    assert(!queue.pop(f));
}

static void reliability() {
    ReliableEndpoint ground(11), air(22); Sink groundSink, airSink;
    assert(air.receive(ground.packet(PacketKind::Poll, true, true), airSink));
    assert(ground.receive(air.packet(PacketKind::Response, true, true), groundSink));
    assert(ground.enqueue(sample()));
    Packet poll = ground.packet(PacketKind::Poll, true, true);
    assert(poll.count == 1 && ground.pending() == 1);
    airSink.blocked = true;
    assert(!air.receive(poll, airSink));
    Packet response = air.packet(PacketKind::Response, true, true);
    assert(response.ackSequence == 0);
    assert(ground.receive(response, groundSink)); assert(ground.pending() == 1);
    airSink.blocked = false;
    assert(air.receive(poll, airSink)); assert(airSink.frames.size() == 1);
    assert(air.receive(poll, airSink)); assert(airSink.frames.size() == 1);
    Packet changed = poll; changed.frames[0].data[0] = 1;
    assert(!air.receive(changed, airSink)); assert(airSink.frames.size() == 1);
    assert(air.enqueue(sample(0x456)));
    response = air.packet(PacketKind::Response, true, true);
    Packet wrongAck = response; wrongAck.ackSession = 99;
    assert(ground.receive(wrongAck, groundSink)); assert(ground.pending() == 1);
    assert(ground.receive(response, groundSink)); assert(ground.pending() == 0);
    assert(groundSink.frames.size() == 1);
    assert(ground.receive(response, groundSink)); assert(groundSink.frames.size() == 1);
    poll = ground.packet(PacketKind::Poll, true, true);
    assert(air.receive(poll, airSink)); assert(air.pending() == 0);
    // A restarted peer starts sequence numbering again under a new session.
    ReliableEndpoint restarted(33);
    assert(air.receive(restarted.packet(PacketKind::Poll, true, true), airSink));
    assert(restarted.receive(air.packet(PacketKind::Response, true, true), groundSink));
    assert(restarted.enqueue(sample(0x321)));
    assert(air.receive(restarted.packet(PacketKind::Poll, true, true), airSink));
    assert(airSink.frames.size() == 2);
    // Silent or unavailable USB must not consume the airborne outbox.
    assert(air.enqueue(sample(0x333)));
    assert(air.packet(PacketKind::Response, false, true).count == 0);
    assert(air.pending() == 1);
}

static void simultaneousStream() {
    ReliableEndpoint g(101), a(202); Sink gs, as;
    assert(a.receive(g.packet(PacketKind::Poll, true, true), as));
    assert(g.receive(a.packet(PacketKind::Response, true, true), gs));
    for (unsigned round = 0; round < 100; ++round) {
        for (unsigned i = 0; i < 4; ++i) {
            assert(g.enqueue(sample(0x100 + i))); assert(a.enqueue(sample(0x200 + i)));
        }
        Packet p = g.packet(PacketKind::Poll, true, true);
        assert(a.receive(p, as));
        Packet r = a.packet(PacketKind::Response, true, true);
        // Lost responses force the identical poll to be retried.
        if (round % 3 == 0) assert(a.receive(p, as));
        assert(g.receive(r, gs));
        assert(a.receive(g.packet(PacketKind::Poll, true, true), as));
        assert(g.pending() == 0 && a.pending() == 0);
        for (unsigned i = 0; i < 4; ++i) {
            CanFrame f; assert(as.frames.pop(f) && f.id == 0x100 + i);
            assert(gs.frames.pop(f) && f.id == 0x200 + i);
        }
        assert(gs.frames.size() == 0 && as.frames.size() == 0);
    }
}

static void rebootDuringUnacknowledgedDelivery() {
    ReliableEndpoint ground(11), air(22); Sink groundSink, oldAirSink, newAirSink;
    // Establish the peer before sending the first command.
    assert(air.receive(ground.packet(PacketKind::Poll, true, true), oldAirSink));
    assert(ground.receive(air.packet(PacketKind::Response, true, true), groundSink));
    assert(ground.enqueue(sample(0x555)));
    Packet delivered = ground.packet(PacketKind::Poll, true, true);
    assert(air.receive(delivered, oldAirSink));
    assert(oldAirSink.frames.size() == 1);
    // The response ACK is lost, and the destination reboots.
    ReliableEndpoint restartedAir(33);
    assert(!restartedAir.receive(delivered, newAirSink));
    assert(newAirSink.frames.size() == 0);
    Packet hello = restartedAir.packet(PacketKind::Response, false, true);
    hello.destinationSession = 11;
    assert(ground.receive(hello, groundSink));
    assert(ground.uncertain() == 1 && ground.pending() == 0);
    assert(ground.enqueue(sample(0x556)));
    assert(restartedAir.receive(ground.packet(PacketKind::Poll, true, true), newAirSink));
    assert(newAirSink.frames.size() == 1);
    CanFrame frame; assert(newAirSink.frames.pop(frame) && frame.id == 0x556);
}

int main() {
    codecs(); queues(); reliability(); simultaneousStream(); rebootDuringUnacknowledgedDelivery();
    puts("CAN codecs, bounded queues, backpressure, duplicate suppression and bidirectional streams passed");
}
