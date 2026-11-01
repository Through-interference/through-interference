#include "radio_hardware.h"
#include "radio_config.h"

SX1262 radio = new Module(
    LORA_CS,
    LORA_DIO1,
    LORA_RST,
    LORA_BUSY
);

volatile bool operationDone = false;

void IRAM_ATTR setFlag(void) {
    operationDone = true;
}
