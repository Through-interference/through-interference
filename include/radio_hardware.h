#pragma once

#include <Arduino.h>
#include <RadioLib.h>

extern SX1262 radio;
extern volatile bool operationDone;

void IRAM_ATTR setFlag(void);
