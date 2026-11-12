// Compile the production BridgeController and protocol against injected hardware.
// The mocks below implement only hardware/AT services, never controller decisions.
#include <string>
#include <cstdio>
#include <cassert>
#include <cstring>
#define private public
#include "BridgeController.h"
#include "RadioChannelManager.h"
#undef private
#include "crypto.h"
#include "radio_hardware.h"

Stream Serial;
unsigned long testMillis = 0;
SX1262 radio;
volatile bool operationDone = false;
void setFlag() { operationDone = true; }
static bool injectCanBeginFailure = false;
static int canBeginCalls = 0;
static bool validIncoming = false;
static canlink::Packet incoming;

AtCommandManager::AtCommandManager(Stream& port, uint8_t pin)
    : _port(port), _rfSwitchPin(pin), _mutex(nullptr) {}
void AtCommandManager::begin() { _cfg.aesKey[0] = 1; }
void AtCommandManager::processInput() {}
ModemConfig AtCommandManager::getCurrentConfig() { return _cfg; }
bool AtCommandManager::takePendingConfig(ModemConfig&) { return false; }
void AtCommandManager::updateTelemetry(const ModemTelemetry&) {}
bool cryptoInit() { return true; }
bool cryptoUpdateKey(const uint8_t*) { return true; }
bool cryptoEncryptBytes(const uint8_t*, size_t, String&) { return true; }
CryptoPacketStatus cryptoDecryptBytes(const String&, uint8_t* data, size_t capacity, size_t& length, uint32_t&, uint64_t&, bool) {
    return validIncoming && canlink::encodePacket(incoming, data, capacity, length) ? CRYPTO_PACKET_NEW : CRYPTO_PACKET_ERROR;
}
bool CanHardware::begin(uint32_t bitrate) {
    ++canBeginCalls;
    installed_ = !injectCanBeginFailure;
    bitrate_ = bitrate;
    return installed_;
}
bool CanHardware::changeBitrate(uint32_t bitrate) { return begin(bitrate); }
void CanHardware::update(canlink::ReliableEndpoint&) {}
bool CanHardware::accept(const canlink::CanFrame*, size_t) { return installed_; }
void CanHardware::diagnostics(Stream&) const {}
static void reset() {
    Serial.output.clear(); radio = SX1262(); operationDone = false;
    testMillis = 0; radioChannelManager.currentChannel_ = 0;
    injectCanBeginFailure = false; canBeginCalls = 0;
    validIncoming = false; incoming = canlink::Packet();
}

static void rxFailureReinitializesRadio(bool airborne) {
    reset();
    BridgeController air(airborne);
    air.begin();
    assert(radio.beginCalls == 1 && radio.receiving);
    canlink::Packet hello; hello.session = 77;
    assert(air.endpoint_.receive(hello, air));
    canlink::CanFrame frame; frame.id = 0x123;
    assert(air.endpoint_.enqueue(frame));
    const canlink::Packet active = air.endpoint_.packet(canlink::PacketKind::Poll, true, true);
    assert(active.count == 1);
    // A successful TX finishes, but the radio cannot reenter RX until reinitialized.
    air.transmitting_ = true;
    radio.fault = true;
    operationDone = true;
    air.update();
    assert(!air.radioReady_ && !air.waiting_ && !air.transmitting_);
    assert(radio.failedReceiveCalls == 1);
    testMillis = air.retryRadioAt_ - 1;
    air.update();
    assert(radio.beginCalls == 1);
    testMillis = air.retryRadioAt_;
    air.update();
    assert(radio.beginCalls == 2 && radio.receiving && air.radioReady_);
    const canlink::Packet retained = air.endpoint_.packet(canlink::PacketKind::Poll, true, true);
    assert(retained.count == 1 && retained.sequence == active.sequence && retained.session == active.session);
    assert(air.endpoint_.peerSession() == 77);
}

static void fhssOffKeepsPrimaryChannel(bool airborne) {
    reset();
    BridgeController air(airborne);
    air.begin();
    assert(radioChannelManager.setChannel(1));
    ModemConfig cfg = air.config_;
    cfg.fhssEnabled = false;
    assert(air.configure(cfg));
    assert(radioChannelManager.currentChannel() == 0);
    testMillis = 10000;
    air.update();
    assert(!air.config_.fhssEnabled && radioChannelManager.currentChannel() == 0);
}

static void failedCanStartRetriesWithBackoff() {
    reset();
    injectCanBeginFailure = true;
    BridgeController air(true);
    air.begin();
    assert(!air.can_.ready());
    Serial.output.clear();
    air.command("CAN STAT", Serial);
    assert(Serial.output.find("CAN_READY=0") != std::string::npos);
    for (int i = 0; i < 1900; ++i) air.update();
    assert(canBeginCalls == 1);
    testMillis = air.retryCanAt_;
    air.update();
    assert(canBeginCalls == 2 && !air.can_.ready());
    injectCanBeginFailure = false;
    testMillis = air.retryCanAt_;
    air.update();
    assert(canBeginCalls == 3 && air.can_.ready());
    Serial.output.clear();
    air.command("CAN STAT", Serial);
    assert(Serial.output.find("CAN_READY=1") != std::string::npos);
    testMillis += 10000;
    air.update();
    assert(canBeginCalls == 3);
}

static void recoverEveryRxEntry() {
    for (int path = 0; path < 5; ++path) {
        reset();
        BridgeController air(true);
        air.begin();
        radio.fault = true;
        if (path == 0) {
            radio.transmitState = -2;
            air.send(canlink::PacketKind::Poll, false);
        } else if (path == 1) {
            air.receive(); // Radio read error.
        } else if (path == 2) {
            radio.readState = 0;
            air.receive(); // Invalid encrypted packet.
        } else if (path == 3) {
            testMillis = 7000;
            air.recoverChannel();
        } else {
            radio.readState = 0;
            validIncoming = true;
            incoming.session = 77;
            air.receive(); // Valid poll; RX fails after scheduling its response.
        }
        assert(!air.radioReady_);
        testMillis = air.retryRadioAt_;
        air.update();
        assert(air.radioReady_ && radio.receiving && radio.beginCalls == 2);
    }
}

static void channelFailureStopsSendingUntilReinitialized() {
    reset();
    BridgeController ground(false);
    ground.begin();
    radio.frequencyFault = true;
    testMillis = 10000;
    ground.update();
    assert(!ground.radioReady_ && !ground.transmitting_);
    assert(ground.radioTx_ == 0);
    radio.frequencyFault = false;
    testMillis = ground.retryRadioAt_;
    ground.update();
    assert(ground.radioReady_ && radio.receiving);
}

int main() {
    rxFailureReinitializesRadio(true);
    rxFailureReinitializesRadio(false);
    fhssOffKeepsPrimaryChannel(true);
    fhssOffKeepsPrimaryChannel(false);
    failedCanStartRetriesWithBackoff();
    recoverEveryRxEntry();
    channelFailureStopsSendingUntilReinitialized();
    std::puts("Bridge RX recovery, CAN retry, fixed channels and error backoff passed");
}
