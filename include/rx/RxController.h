#pragma once
#include "BridgeController.h"
class RxController : public BridgeController {
public:
    RxController() : BridgeController(true) {}
};
