#pragma once
#include "activation.h"
#include "platform_activation.h"
#include <Arduino.h>

namespace ActivationService {
bool required();
void init();
rid_activation::Snapshot snapshot();
bool business_allowed();
const char *failure();
String status_json();
platform_activation::State platform_state();
bool factory_provision_available();
rid_activation::Result provision(const char sn[20], uint8_t aircraft_class);
rid_activation::Result synchronize(const char sn[20], bool active, uint64_t transaction);
}
