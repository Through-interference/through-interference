#pragma once

#include <Arduino.h>

// ESP32 DevKit V1: SPI pins.
static const uint8_t LORA_SCK  = 18;
static const uint8_t LORA_MISO = 19;
static const uint8_t LORA_MOSI = 23;
static const uint8_t LORA_CS   = 5;

// E22-900M30S / SX1262.
static const uint8_t LORA_RST  = 14;
static const uint8_t LORA_BUSY = 26;
static const uint8_t LORA_DIO1 = 33;
static const uint8_t LORA_RXEN = 32;
static const uint8_t LORA_TXEN = 25;

// Application-level node IDs. Keep identical on both boards.
static const uint32_t TX_NODE_ID = 1;
static const uint32_t RX_NODE_ID = 2;

//constexpr uint32_t TOTAL_MESSAGES = 1;