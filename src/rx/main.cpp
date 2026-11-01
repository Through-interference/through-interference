#include "rx/RxController.h"

RxController receiver;

void setup() {
    receiver.begin();
}

void loop() {
    receiver.update();
}
