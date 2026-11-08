#include "CanHardware.h"
#include <Arduino.h>
#include <string.h>

bool CanHardware::begin(uint32_t bitrate) {
    twai_timing_config_t timing;
    switch (bitrate) {
        case 125000: timing = TWAI_TIMING_CONFIG_125KBITS(); break;
        case 250000: timing = TWAI_TIMING_CONFIG_250KBITS(); break;
        case 500000: timing = TWAI_TIMING_CONFIG_500KBITS(); break;
        case 1000000: timing = TWAI_TIMING_CONFIG_1MBITS(); break;
        default: return false;
    }
    twai_general_config_t general = TWAI_GENERAL_CONFIG_DEFAULT(GPIO_NUM_4, GPIO_NUM_17, TWAI_MODE_NORMAL);
    general.tx_queue_len = 1; general.rx_queue_len = 128;
    general.alerts_enabled = TWAI_ALERT_TX_SUCCESS | TWAI_ALERT_TX_FAILED | TWAI_ALERT_BUS_OFF |
        TWAI_ALERT_BUS_RECOVERED | TWAI_ALERT_RX_QUEUE_FULL;
    twai_filter_config_t filter = TWAI_FILTER_CONFIG_ACCEPT_ALL();
    if (twai_driver_install(&general, &timing, &filter) != ESP_OK) return false;
    if (twai_start() != ESP_OK) { twai_driver_uninstall(); return false; }
    installed_ = true; recovering_ = false; awaitingTx_ = false; bitrate_ = bitrate;
    Serial.printf("[CAN] Started TX=GPIO4 RX=GPIO17 bitrate=%lu\n", static_cast<unsigned long>(bitrate));
    return true;
}
bool CanHardware::changeBitrate(uint32_t bitrate) {
    if (bitrate == bitrate_ && installed_) return true;
    if (pending() || awaitingTx_ || recovering_) return false;
    const uint32_t previous = bitrate_;
    if (installed_) { twai_stop(); twai_driver_uninstall(); installed_ = false; }
    if (begin(bitrate)) return true;
    if (previous) begin(previous);
    return false;
}
bool CanHardware::accept(const canlink::CanFrame* frames, size_t count) {
    if (!installed_ || tx_.free() < count) return false;
    for (size_t i = 0; i < count; ++i) if (!canlink::validFrame(frames[i])) return false;
    for (size_t i = 0; i < count; ++i) tx_.push(frames[i]);
    return true;
}
void CanHardware::update(canlink::ReliableEndpoint& endpoint) {
    if (!installed_) return;
    const unsigned long now = millis();
    uint32_t alerts = 0;
    if (twai_read_alerts(&alerts, 0) == ESP_OK) {
        if ((alerts & TWAI_ALERT_TX_SUCCESS) && awaitingTx_) {
            canlink::CanFrame frame; tx_.pop(frame); awaitingTx_ = false; ++transmitted;
        }
        if (alerts & TWAI_ALERT_TX_FAILED) { awaitingTx_ = false; ++failures; nextTxAt_ = now + 50; }
        if (alerts & TWAI_ALERT_BUS_OFF) {
            ++busOffs; awaitingTx_ = false; recovering_ = true;
            twai_initiate_recovery();
        }
        if (alerts & TWAI_ALERT_BUS_RECOVERED) {
            recovering_ = twai_start() != ESP_OK; nextTxAt_ = now + 50;
        }
    }
    twai_status_info_t status;
    if (twai_get_status_info(&status) == ESP_OK) dropped = status.rx_missed_count + status.rx_overrun_count;
    // Stop draining before the radio queue is full. Hardware overflow is counted above.
    for (size_t i = 0; i < 16 && endpoint.free(); ++i) {
        twai_message_t message = {};
        if (twai_receive(&message, 0) != ESP_OK) break;
        canlink::CanFrame frame;
        frame.id = message.identifier; frame.dlc = message.data_length_code;
        frame.extended = message.extd; frame.remote = message.rtr;
        if (frame.dlc > 8) { ++dropped; continue; }
        if (!frame.remote) memcpy(frame.data, message.data, frame.dlc);
        if (endpoint.enqueue(frame)) ++received; else ++dropped;
    }
    if (recovering_) return;
    if (awaitingTx_ && now - txAt_ > 250) {
        // A missing CAN ACK must not block the radio loop or discard the frame.
        twai_stop(); twai_clear_transmit_queue(); twai_start();
        awaitingTx_ = false; ++failures; nextTxAt_ = now + 50;
    }
    if (!awaitingTx_ && static_cast<int32_t>(now - nextTxAt_) >= 0) {
        canlink::CanFrame frame;
        if (!tx_.peek(frame)) return;
        twai_message_t message = {};
        message.identifier = frame.id; message.extd = frame.extended;
        message.rtr = frame.remote; message.data_length_code = frame.dlc;
        message.ss = 1; // One hardware attempt, then retry from our retained queue.
        if (!frame.remote) memcpy(message.data, frame.data, frame.dlc);
        if (twai_transmit(&message, 0) == ESP_OK) { awaitingTx_ = true; txAt_ = now; }
        else { ++failures; nextTxAt_ = now + 50; }
    }
}
