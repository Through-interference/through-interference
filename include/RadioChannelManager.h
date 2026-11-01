#pragma once

#include <Arduino.h>
#include "frequency_config.h"

class RadioChannelManager {
public:
    bool setChannel(uint8_t channel);
    bool switchToNext();

    bool startSearch(unsigned long dwellMs);
    bool updateSearch();
    void stopSearch();

    bool isSearching() const;

    uint8_t currentChannel() const;
    float currentFrequencyMHz() const;

private:
    uint8_t currentChannel_ = RADIO_PRIMARY_CHANNEL;

    bool searching_ = false;

    unsigned long searchDwellMs_ = 0;
    unsigned long nextSearchAt_ = 0;
};

extern RadioChannelManager radioChannelManager;
