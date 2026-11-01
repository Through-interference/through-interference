#include "tx/TxController.h"

TxController transmitter;

void setup() {
    transmitter.begin();
}

void loop() {
    transmitter.update();
}
