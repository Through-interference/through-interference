#pragma once

#include <Arduino.h>

// Количество рабочих каналов.
constexpr uint8_t RADIO_CHANNEL_COUNT = 2;

// Индексы каналов.
constexpr uint8_t RADIO_PRIMARY_CHANNEL = 0;
constexpr uint8_t RADIO_BACKUP_CHANNEL = 1;

// Центральные частоты в МГц.
static const float RADIO_CHANNELS_MHZ[RADIO_CHANNEL_COUNT] = {
    868.0f,
    869.1f
};

// TX: время работы на одном канале во время поиска.
constexpr unsigned long TX_CHANNEL_DWELL_MS =9000UL;

// TX: интервал повторной отправки сохранённого DATA.
constexpr unsigned long TX_SEARCH_REPEAT_MS = 1500UL;

// RX: сколько времени слушать каждый канал.
constexpr unsigned long RX_SCAN_DWELL_MS = 3000UL;

// RX: начать поиск, если DATA давно не поступает.
constexpr unsigned long RX_SCAN_AFTER_IDLE_MS = 6000UL;

// RX: начать поиск после такого количества подряд
// полученных дубликатов DATA (после отправки ACK).
constexpr uint8_t RX_DUPLICATES_BEFORE_SCAN = 2;