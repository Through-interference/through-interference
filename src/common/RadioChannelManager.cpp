#include "RadioChannelManager.h"

#include <RadioLib.h>
#include "radio_hardware.h"

RadioChannelManager radioChannelManager;

RadioChannelManager::RadioChannelManager() {
    for (uint8_t i = 0; i < RADIO_CHANNEL_COUNT; i++) {
        channelsMHz_[i] = RADIO_CHANNELS_MHZ[i];
    }
}

bool RadioChannelManager::setChannel(uint8_t channel) {
    if (channel >= RADIO_CHANNEL_COUNT) {
        return false;
    }

    if (channel == currentChannel_) {
        return true;
    }

    radio.standby();

    const float frequencyMHz = channelsMHz_[channel];

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

bool RadioChannelManager::setPrimaryFrequency(float freqMHz) {
    if (freqMHz < 850.0f || freqMHz > 930.0f) {
        return false;
    }

    if (currentChannel_ == RADIO_PRIMARY_CHANNEL) {
        radio.standby();

        const int state = radio.setFrequency(freqMHz);

        if (state != RADIOLIB_ERR_NONE) {
            Serial.print("[RADIO] Primary frequency apply failed: ");
            Serial.println(state);
            return false;
        }
    }

    channelsMHz_[RADIO_PRIMARY_CHANNEL] = freqMHz;

    Serial.print("[RADIO] Primary frequency set to ");
    Serial.print(freqMHz);
    Serial.println(" MHz");

    return true;
}
bool RadioChannelManager::setBackupFrequency(float freqMHz) {
    constexpr uint8_t BACKUP_CHANNEL = 1;

    if (RADIO_CHANNEL_COUNT <= BACKUP_CHANNEL ||
        freqMHz < 850.0f ||
        freqMHz > 930.0f) {
        return false;
    }

    if (currentChannel_ == BACKUP_CHANNEL) {
        radio.standby();

        const int state = radio.setFrequency(freqMHz);

        if (state != RADIOLIB_ERR_NONE) {
            Serial.print("[RADIO] Backup frequency apply failed: ");
            Serial.println(state);
            return false;
        }
    }

    channelsMHz_[BACKUP_CHANNEL] = freqMHz;

    Serial.print("[RADIO] Backup frequency set to ");
    Serial.print(freqMHz);
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
}

bool RadioChannelManager::isSearching() const {
    return searching_;
}

uint8_t RadioChannelManager::currentChannel() const {
    return currentChannel_;
}

float RadioChannelManager::currentFrequencyMHz() const {
    return channelsMHz_[currentChannel_];
}