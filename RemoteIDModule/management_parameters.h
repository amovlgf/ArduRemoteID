#pragma once
#include "parameters.h"
#include <management_protocol.h>

namespace ManagedParameters {
// Load immutable ROMFS defaults once during startup, outside the CAN handler.
void initialize();
uint16_t count();
bool describe(uint16_t index, amov_device::Response &response);
bool describe(const char *name, amov_device::Response &response);
amov_device::Result validate(const amov_device::Request &request);
// Worker only; does not publish to RAM. All legacy writers must be excluded.
amov_device::Result persist(const amov_device::Request &request);
// Main/CAN loop only, after a verified persistence result.
void apply(const amov_device::Request &request);
bool reset();
// Used to resolve a lost completion after reboot without retaining passwords.
bool stored_digest(const char *name, uint8_t out[32]);
void value_digest(amov_device::Type type, const uint8_t *value, uint8_t size, uint8_t out[32]);
}
