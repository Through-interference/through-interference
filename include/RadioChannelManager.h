#pragma once

#include <Arduino.h>
#include "frequency_config.h"

class RadioChannelManager {
public:
    RadioChannelManager();

    bool setChannel(uint8_t channel);
    bool switchToNext();

    bool startSearch(unsigned long dwellMs);
    bool updateSearch();
    void stopSearch();

    bool isSearching() const;
    uint8_t currentChannel() const;
    float currentFrequencyMHz() const;

    //для смены базовой и резервной частот на лету
    bool setPrimaryFrequency(float freqMHz);
    bool setBackupFrequency(float freqMHz);

private:
    uint8_t currentChannel_ = RADIO_PRIMARY_CHANNEL;
    bool searching_ = false;
    unsigned long searchDwellMs_ = 0;
    unsigned long nextSearchAt_ = 0;

    float channelsMHz_[RADIO_CHANNEL_COUNT];
};

extern RadioChannelManager radioChannelManager;