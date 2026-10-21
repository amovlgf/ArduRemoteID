#pragma once
#include "activation.h"
#include <Arduino.h>

namespace ActivationService {
bool required();
void init();
rid_activation::Snapshot snapshot();
bool business_allowed();
const char *failure();
// One outstanding upload, copied bounded storage; never changes live identity.
bool submit(const uint8_t *bytes, size_t size);
// Only the low-priority web task calls this, outside HTTP callbacks.
void process();
String status_json();
bool clear_pending_for_defaults();
}
