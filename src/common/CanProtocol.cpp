#include "CanProtocol.h"
#include <string.h>

namespace canlink {
namespace {
int nibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
bool readHex(const char* input, size_t count, uint32_t& output) {
    output = 0;
    for (size_t i = 0; i < count; ++i) {
        int n = nibble(input[i]); if (n < 0) return false;
        output = (output << 4) | static_cast<uint32_t>(n);
    }
    return true;
}
void put32(uint8_t* p, uint32_t n) {
    for (size_t i = 0; i < 4; ++i) p[i] = static_cast<uint8_t>(n >> (8 * i));
}
uint32_t get32(const uint8_t* p) {
    uint32_t n = 0;
    for (size_t i = 0; i < 4; ++i) n |= static_cast<uint32_t>(p[i]) << (8 * i);
    return n;
}
void put64(uint8_t* p, uint64_t n) {
    put32(p, static_cast<uint32_t>(n)); put32(p + 4, static_cast<uint32_t>(n >> 32));
}
uint64_t get64(const uint8_t* p) {
    return get32(p) | (static_cast<uint64_t>(get32(p + 4)) << 32);
}
bool validPacket(const Packet& p) {
    if (p.kind != PacketKind::Poll && p.kind != PacketKind::Response) return false;
    if (!p.session || p.count > BatchSize || (p.count == 0) != (p.sequence == 0)) return false;
    if (p.count && !p.destinationSession) return false;
    if ((p.ackSession == 0) != (p.ackSequence == 0)) return false;
    for (size_t i = 0; i < p.count; ++i) if (!validFrame(p.frames[i])) return false;
    return true;
}
}

bool validFrame(const CanFrame& f) {
    return f.dlc <= 8 && f.id <= (f.extended ? 0x1fffffffUL : 0x7ffUL);
}
bool equalFrame(const CanFrame& a, const CanFrame& b) {
    return a.id == b.id && a.dlc == b.dlc && a.extended == b.extended &&
           a.remote == b.remote && (a.remote || memcmp(a.data, b.data, a.dlc) == 0);
}
bool parseCanFrame(const char* text, CanFrame& frame) {
    if (!text) return false;
    const size_t length = strlen(text);
    if (length < 5) return false;
    CanFrame f;
    if (text[0] != 't' && text[0] != 'T' && text[0] != 'r' && text[0] != 'R') return false;
    f.extended = text[0] == 'T' || text[0] == 'R';
    f.remote = text[0] == 'r' || text[0] == 'R';
    const size_t digits = f.extended ? 8 : 3;
    if (length < digits + 2 || !readHex(text + 1, digits, f.id)) return false;
    if (text[digits + 1] < '0' || text[digits + 1] > '8') return false;
    f.dlc = static_cast<uint8_t>(text[digits + 1] - '0');
    if (!validFrame(f) || length != digits + 2 + (f.remote ? 0 : 2 * f.dlc)) return false;
    if (!f.remote) for (size_t i = 0; i < f.dlc; ++i) {
        uint32_t byte;
        if (!readHex(text + digits + 2 + 2 * i, 2, byte)) return false;
        f.data[i] = static_cast<uint8_t>(byte);
    }
    frame = f; return true;
}
bool formatCanFrame(const CanFrame& f, char* out, size_t capacity) {
    if (!out || !validFrame(f)) return false;
    const size_t digits = f.extended ? 8 : 3;
    const size_t length = digits + 2 + (f.remote ? 0 : 2 * f.dlc);
    if (capacity <= length) return false;
    const char* hex = "0123456789ABCDEF";
    out[0] = f.remote ? (f.extended ? 'R' : 'r') : (f.extended ? 'T' : 't');
    for (size_t i = 0; i < digits; ++i) out[1 + i] = hex[(f.id >> (4 * (digits - i - 1))) & 15];
    out[digits + 1] = static_cast<char>('0' + f.dlc);
    if (!f.remote) for (size_t i = 0; i < f.dlc; ++i) {
        out[digits + 2 + 2 * i] = hex[f.data[i] >> 4];
        out[digits + 3 + 2 * i] = hex[f.data[i] & 15];
    }
    out[length] = 0; return true;
}
bool encodePacket(const Packet& p, uint8_t* out, size_t capacity, size_t& length) {
    length = 0;
    if (!out || !validPacket(p) || capacity < 36 + 14 * p.count) return false;
    out[0] = 2; out[1] = static_cast<uint8_t>(p.kind);
    put64(out + 2, p.session); put32(out + 10, p.sequence);
    put64(out + 14, p.destinationSession);
    put64(out + 22, p.ackSession); put32(out + 30, p.ackSequence);
    out[34] = p.count; out[35] = p.ready ? 1 : 0;
    for (size_t i = 0; i < p.count; ++i) {
        const CanFrame& f = p.frames[i]; uint8_t* dest = out + 36 + i * 14;
        put32(dest, f.id); dest[4] = (f.extended ? 1 : 0) | (f.remote ? 2 : 0); dest[5] = f.dlc;
        memset(dest + 6, 0, 8);
        if (!f.remote) memcpy(dest + 6, f.data, f.dlc);
    }
    length = 36 + 14 * p.count; return true;
}
bool decodePacket(const uint8_t* input, size_t length, Packet& packet) {
    if (!input || length < 36 || input[0] != 2 || input[34] > BatchSize || input[35] > 1 ||
        length != 36 + 14 * input[34]) return false;
    Packet p; p.kind = static_cast<PacketKind>(input[1]);
    p.session = get64(input + 2); p.sequence = get32(input + 10);
    p.destinationSession = get64(input + 14);
    p.ackSession = get64(input + 22); p.ackSequence = get32(input + 30);
    p.count = input[34]; p.ready = input[35] != 0;
    for (size_t i = 0; i < p.count; ++i) {
        const uint8_t* src = input + 36 + i * 14; CanFrame& f = p.frames[i];
        if (src[4] & ~3) return false;
        f.id = get32(src); f.extended = src[4] & 1; f.remote = src[4] & 2; f.dlc = src[5];
        if (!validFrame(f)) return false;
        if (!f.remote) memcpy(f.data, src + 6, f.dlc);
    }
    if (!validPacket(p)) return false;
    packet = p; return true;
}
}
