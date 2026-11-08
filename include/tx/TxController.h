#pragma once
#include "BridgeController.h"
class TxController : public BridgeController {
public:
    TxController() : BridgeController(false) {}
};
