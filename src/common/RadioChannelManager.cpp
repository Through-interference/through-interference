#include "RadioChannelManager.h"

#include <RadioLib.h>
#include "radio_hardware.h"

RadioChannelManager radioChannelManager;


bool RadioChannelManager::setChannel(uint8_t channel) {
    if (channel >= RADIO_CHANNEL_COUNT) {
        return false;
    }

    if (channel == currentChannel_) {
        return true;
    }

    radio.standby();

    // RadioLib API in this project uses MHz.
    const float frequencyMHz = RADIO_CHANNELS_MHZ[channel];

    Serial.print("[RADIO] Changing frequency to ");
    Serial.print(frequencyMHz);
    Serial.println(" MHz");

    const int state = radio.setFrequency(frequencyMHz);

    if (state != RADIOLIB_ERR_NONE) {
        Serial.print("[RADIO] Frequency change failed: ");
        Serial.println(state);
        return false;
    }

    currentChannel_ = channel;

    Serial.print("[RADIO] Channel: ");
    Serial.print(currentChannel_);
    Serial.print(" | Frequency: ");
    Serial.print(frequencyMHz);
    Serial.println(" MHz");

    return true;
}

bool RadioChannelManager::switchToNext() {
    const uint8_t nextChannel =
        (currentChannel_ + 1) % RADIO_CHANNEL_COUNT;

    return setChannel(nextChannel);
}


bool RadioChannelManager::startSearch(unsigned long dwellMs) {
    if (dwellMs == 0 || searching_) {
        return false;
    }

    searchDwellMs_ = dwellMs;

    if (!switchToNext()) {
        return false;
    }

    searching_ = true;
    nextSearchAt_ = millis() + searchDwellMs_;

    Serial.print("[RADIO] Search started on channel ");
    Serial.println(currentChannel_);

    return true;
}


bool RadioChannelManager::updateSearch() {
    if (!searching_) {
        return false;
    }

    if ((long)(millis() - nextSearchAt_) < 0) {
        return false;
    }

    if (!switchToNext()) {
        nextSearchAt_ = millis() + searchDwellMs_;
        return false;
    }

    nextSearchAt_ = millis() + searchDwellMs_;

    return true;
}


void RadioChannelManager::stopSearch() {
    searching_ = false;

    Serial.print("[RADIO] Search stopped on channel ");
    Serial.println(currentChannel_);
}


bool RadioChannelManager::isSearching() const {
    return searching_;
}


uint8_t RadioChannelManager::currentChannel() const {
    return currentChannel_;
}


float RadioChannelManager::currentFrequencyMHz() const {
    return RADIO_CHANNELS_MHZ[currentChannel_];
}
