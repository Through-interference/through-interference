#include "BridgeController.h"
#include "crypto.h"
#include "radio_config.h"
#include "radio_hardware.h"
#include "RadioChannelManager.h"
#include <stdlib.h>
#include <string.h>

namespace {
bool due(unsigned long now, unsigned long at) { return static_cast<int32_t>(now - at) >= 0; }
bool keyPresent(const ModemConfig& c) {
    for (uint8_t b : c.aesKey) if (b) return true;
    return false;
}
bool parseId(const String& text, uint32_t& value) {
    if (!text.length()) return false;
    uint64_t n = 0;
    for (size_t i = 0; i < text.length(); ++i) {
        if (text[i] < '0' || text[i] > '9') return false;
        n = n * 10 + text[i] - '0'; if (n > UINT32_MAX) return false;
    }
    value = static_cast<uint32_t>(n); return true;
}
}

void BridgeController::begin() {
    Serial.begin(115200); delay(300);
    const uint64_t session = (static_cast<uint64_t>(esp_random()) << 32) | esp_random();
    endpoint_ = canlink::ReliableEndpoint(session);
    at_.setExtensionHandler(extension, this); at_.setCanBaudHandler(canBaud); at_.begin();
    config_ = at_.getCurrentConfig();
    ModemConfig initial; at_.takePendingConfig(initial);
    cryptoReady_ = cryptoInit();
    if (airborne_) startCan();
    configure(config_);
    lastChannelAt_ = millis();
    Serial.printf("+READY: %s CANLINK=2\n", airborne_ ? "AIR" : "GROUND");
}

bool BridgeController::configure(const ModemConfig& c) {
    radioReady_ = false; transmitting_ = waiting_ = responsePending_ = false;
    operationDone = false; radio.standby(); config_ = c;
    if (!cryptoReady_ || !keyPresent(c)) {
        Serial.println("ERROR: UNBOUND_USE_KEY_SAVE"); retryRadioAt_ = millis() + 2000; return false;
    }
    int state;
    const float frequency = !c.fhssEnabled || radioChannelManager.currentChannel() == 0 ? c.primFreqMhz : c.backFreqMhz;
    radio.setRfSwitchPins(LORA_RXEN, LORA_TXEN);
    switch (c.modulation) {
        case classModulation::GFSK100:
            state = radio.beginFSK(frequency, 100.0f, 50.0f, 234.3f, c.txPowerDbm, 16, 1.8f); break;
        case classModulation::GFSK250:
            state = radio.beginFSK(frequency, 250.0f, 125.0f, 467.0f, c.txPowerDbm, 16, 1.8f); break;
        case classModulation::LORA_SF7:
        case classModulation::LORA_SF9:
            state = radio.begin(frequency, 125.0f, c.modulation == classModulation::LORA_SF7 ? 7 : 9,
                                5, RADIOLIB_SX126X_SYNC_WORD_PRIVATE, c.txPowerDbm, 8, 1.8f); break;
        default: state = -1;
    }
    if (state == RADIOLIB_ERR_NONE && !cryptoUpdateKey(c.aesKey)) state = -1;
    if (state == RADIOLIB_ERR_NONE && !c.fhssEnabled &&
        !radioChannelManager.setChannel(RADIO_PRIMARY_CHANNEL)) state = -1;
    if (state == RADIOLIB_ERR_NONE && !radioChannelManager.setPrimaryFrequency(c.primFreqMhz)) state = -1;
    if (state == RADIOLIB_ERR_NONE && !radioChannelManager.setBackupFrequency(c.backFreqMhz)) state = -1;
    radio.setRfSwitchPins(LORA_RXEN, LORA_TXEN); radio.setDio1Action(setFlag);
    if (state == RADIOLIB_ERR_NONE) state = radio.startReceive();
    radioReady_ = state == RADIOLIB_ERR_NONE;
    if (!radioReady_) Serial.printf("ERROR: RADIO_START=%d\n", state);
    else Serial.println("+RADIO: READY");
    retryRadioAt_ = millis() + 2000; nextPollAt_ = millis() + 30;
    return radioReady_;
}

void BridgeController::radioFailure(const char* stage, int state) {
    ++timeouts_;
    radioReady_ = transmitting_ = waiting_ = responsePending_ = false;
    operationDone = false;
    radio.standby();
    retryRadioAt_ = millis() + 250;
    Serial.printf("ERROR: RADIO_%s=%d\n", stage, state);
}

bool BridgeController::startReceive() {
    const int state = radio.startReceive();
    if (state == RADIOLIB_ERR_NONE) return true;
    radioFailure("RX", state);
    return false;
}

void BridgeController::startCan() {
    if (!can_.begin(config_.canBaud)) Serial.println("ERROR: CAN_START_FAILED");
    retryCanAt_ = millis() + 2000;
}

unsigned long BridgeController::receiveWindow() const {
    const unsigned long airTime = radio.getTimeOnAir(254) / 1000UL;
    return airTime + 250UL < 350UL ? 350UL : airTime + 250UL;
}

bool BridgeController::accept(const canlink::CanFrame* frames, size_t count) {
    if (airborne_) return can_.accept(frames, count);
    if (!usbOpen_ || usbRx_.free() < count) return false;
    for (size_t i = 0; i < count; ++i) usbRx_.push(frames[i]);
    return true;
}

void BridgeController::send(canlink::PacketKind kind, bool includePayload) {
    const bool ready = airborne_ ? can_.ready() && can_.pending() <= 64 - canlink::BatchSize :
        usbOpen_ && usbRx_.free() >= canlink::BatchSize;
    if (airborne_ && endpoint_.peerSession() != replyDestination_) includePayload = false;
    canlink::Packet packet = endpoint_.packet(kind, includePayload, ready);
    if (airborne_) packet.destinationSession = replyDestination_;
    uint8_t bytes[canlink::PacketCapacity]; size_t length;
    String encrypted;
    if (!canlink::encodePacket(packet, bytes, sizeof(bytes), length) ||
        !cryptoEncryptBytes(bytes, length, encrypted)) { ++badPackets_; return; }
    radio.standby(); operationDone = false;
    const int state = radio.startTransmit(encrypted);
    if (state == RADIOLIB_ERR_NONE) {
        transmitting_ = true; txAt_ = millis(); ++radioTx_;
    } else {
        ++timeouts_; startReceive(); nextPollAt_ = millis() + 100;
    }
}

void BridgeController::receive() {
    String encrypted;
    const int state = radio.readData(encrypted);
    if (state != RADIOLIB_ERR_NONE) { ++badPackets_; startReceive(); return; }
    uint8_t bytes[canlink::PacketCapacity]; size_t length = 0; uint32_t sender = 0; uint64_t counter = 0;
    const CryptoPacketStatus status = cryptoDecryptBytes(encrypted, bytes, sizeof(bytes), length, sender, counter, false);
    canlink::Packet packet;
    const canlink::PacketKind expected = airborne_ ? canlink::PacketKind::Poll : canlink::PacketKind::Response;
    if (status != CRYPTO_PACKET_NEW ||
        !canlink::decodePacket(bytes, length, packet) || packet.kind != expected) {
        ++badPackets_; startReceive(); return;
    }
    ++radioRx_; lastPeerAt_ = millis(); peerReady_ = packet.ready;
    endpoint_.receive(packet, *this);
    if (airborne_) { replyDestination_ = packet.session; responsePending_ = true; replyAt_ = millis() + 8; }
    else { waiting_ = false; nextPollAt_ = millis() + (endpoint_.pending() || packet.count ? 2 : 50); }
    startReceive();
}

void BridgeController::serviceUsb() {
    if (airborne_) return;
    const unsigned long now = millis();
    if (usbOpen_ && now - usbLeaseAt_ > 5000) { usbOpen_ = false; Serial.println("+CAN: CLOSED HOST_TIMEOUT"); }
    if (!usbOpen_ || !due(now, nextUsbAt_)) return;
    canlink::CanFrame frame;
    if (!usbRx_.peek(frame)) return;
    char line[32];
    if (canlink::formatCanFrame(frame, line, sizeof(line)))
        Serial.printf("+CAN RX %lu %s\n", static_cast<unsigned long>(usbDeliveryId_), line);
    nextUsbAt_ = now + 250;
}

void BridgeController::recoverChannel() {
    if (!config_.fhssEnabled || transmitting_ || responsePending_ || waiting_) return;
    const unsigned long now = millis();
    const unsigned long idle = lastPeerAt_ ? now - lastPeerAt_ : now;
    const unsigned long dwell = airborne_ ? 6000 : 9000;
    if (idle < 5000 || now - lastChannelAt_ < dwell) return;
    lastChannelAt_ = now;
    if (radioChannelManager.switchToNext()) {
        operationDone = false; startReceive(); nextPollAt_ = now + 20;
    } else radioFailure("CHANNEL", -1);
}

void BridgeController::update() {
    at_.processInput();
    if (endpoint_.uncertain() != reportedUncertain_) {
        reportedUncertain_ = endpoint_.uncertain();
        Serial.printf("+CAN UNCERTAIN %lu\n", static_cast<unsigned long>(reportedUncertain_));
    }
    if (airborne_) {
        if (!can_.ready() && due(millis(), retryCanAt_)) startCan();
        can_.update(endpoint_);
    }
    serviceUsb();
    const unsigned long now = millis();
    if (radioReady_ && operationDone) {
        operationDone = false;
        if (transmitting_) {
            const int state = radio.finishTransmit(); transmitting_ = false;
            const bool receiving = startReceive();
            if (!airborne_ && state == RADIOLIB_ERR_NONE && receiving) { waiting_ = true; deadline_ = millis() + receiveWindow(); }
            else if (state != RADIOLIB_ERR_NONE) { ++timeouts_; nextPollAt_ = millis() + 100; }
        } else receive();
    }
    if (transmitting_ && now - txAt_ > 6000) {
        radioFailure("TX_TIMEOUT", -1);
    }
    if (waiting_ && due(now, deadline_)) { waiting_ = false; ++timeouts_; nextPollAt_ = now + 20; }
    if (!transmitting_ && !waiting_ && !responsePending_) {
        ModemConfig pending;
        if (at_.takePendingConfig(pending)) configure(pending);
        else if (!radioReady_ && due(now, retryRadioAt_)) configure(config_);
    }
    if (radioReady_ && !transmitting_) {
        recoverChannel();
        if (radioReady_ && airborne_ && responsePending_ && due(now, replyAt_)) {
            responsePending_ = false; send(canlink::PacketKind::Response, peerReady_);
        } else if (radioReady_ && !airborne_ && !waiting_ && due(now, nextPollAt_)) {
            nextPollAt_ = now + 100; send(canlink::PacketKind::Poll, true);
        }
    }
    const bool connected = radioReady_ && lastPeerAt_ && millis() - lastPeerAt_ < 3 * receiveWindow();
    if (connected != connected_) {
        connected_ = connected; Serial.printf("+CAN LINK %s\n", connected ? "UP" : "DOWN");
    }
    ModemTelemetry telemetry;
    telemetry.txPackets = radioTx_; telemetry.rxPackets = radioRx_;
    telemetry.linkState = connected_ ? classLinkState::TRACKING : classLinkState::LOST;
    if (radioReady_) { telemetry.rssiDbm = radio.getRSSI(); telemetry.snrDb = radio.getSNR(); }
    at_.updateTelemetry(telemetry);
    delay(1);
}

bool BridgeController::extension(void* context, const String& line, Stream& port) {
    return static_cast<BridgeController*>(context)->command(line, port);
}
bool BridgeController::canBaud(void* context, uint32_t bitrate) {
    BridgeController* self = static_cast<BridgeController*>(context);
    if (!self->airborne_ || !self->can_.changeBitrate(bitrate)) return false;
    self->config_.canBaud = bitrate;
    return true;
}
bool BridgeController::command(const String& line, Stream& port) {
    if (!line.startsWith("CAN ")) return false;
    if (line == "CAN DIAG") {
        if (airborne_) can_.diagnostics(port);
        else port.println("ERROR: CAN_DIAG_AIR_ONLY");
        return true;
    }
    if (line == "CAN STAT") {
        port.printf("+CAN STAT: ROLE=%s,OPEN=%u,LINK=%s,CAN_READY=%u,PEER_READY=%u,TX_PENDING=%u,USB_PENDING=%u,USB_RX=%lu,CAN_RX=%lu,CAN_TX=%lu,CAN_PENDING=%u,CAN_DROP=%lu,CAN_FAIL=%lu,BUS_OFF=%lu,RF_TX=%lu,RF_RX=%lu,RF_TIMEOUT=%lu,RF_BAD=%lu,QUEUE_REJECT=%lu,UNCERTAIN=%lu\r\nOK\r\n",
            airborne_ ? "AIR" : "GROUND", usbOpen_, connected_ ? "UP" : "DOWN", can_.ready(), peerReady_,
            static_cast<unsigned>(endpoint_.pending()), static_cast<unsigned>(usbRx_.size()),
            static_cast<unsigned long>(usbDelivered_), static_cast<unsigned long>(can_.received), static_cast<unsigned long>(can_.transmitted),
            static_cast<unsigned>(can_.pending()), static_cast<unsigned long>(can_.dropped), static_cast<unsigned long>(can_.failures),
            static_cast<unsigned long>(can_.busOffs), static_cast<unsigned long>(radioTx_), static_cast<unsigned long>(radioRx_),
            static_cast<unsigned long>(timeouts_), static_cast<unsigned long>(badPackets_), static_cast<unsigned long>(queueRejected_),
            static_cast<unsigned long>(endpoint_.uncertain()));
        return true;
    }
    if (airborne_) { port.println("ERROR: CAN_USB_GROUND_ONLY"); return true; }
    if (line == "CAN OPEN") { usbOpen_ = true; usbLeaseAt_ = millis(); port.println("OK"); return true; }
    if (line == "CAN CLOSE") { usbOpen_ = false; port.println("OK"); return true; }
    if (line.startsWith("CAN ACK ")) {
        uint32_t id;
        if (!parseId(line.substring(8), id)) { port.println("ERROR: CAN_ACK_ID"); return true; }
        usbLeaseAt_ = millis();
        canlink::CanFrame frame;
        if (id == usbDeliveryId_ && usbRx_.pop(frame)) {
            ++usbDelivered_; if (!++usbDeliveryId_) usbDeliveryId_ = 1; nextUsbAt_ = millis();
        }
        port.println("OK"); return true;
    }
    if (line.startsWith("CAN TX ")) {
        if (!usbOpen_) { port.println("ERROR: CAN_CLOSED"); return true; }
        canlink::CanFrame frame;
        if (!canlink::parseCanFrame(line.substring(7).c_str(), frame)) port.println("ERROR: CAN_FRAME");
        else if (!endpoint_.enqueue(frame)) { ++queueRejected_; port.println("ERROR: CAN_QUEUE_FULL"); }
        else { usbLeaseAt_ = millis(); port.println("+CAN QUEUED"); port.println("OK"); }
        return true;
    }
    port.println("ERROR: CAN_COMMAND"); return true;
}
