#pragma once
#include <stddef.h>
#include <stdint.h>
namespace ManagementService {
void init();
// Main CAN task: complete RAM publication and delayed reboot.
void update();
// Dedicated low-priority network task: bounded mutation queue, NVS and verification.
void process();
bool busy();
bool handle(const uint8_t *request, size_t size, uint8_t *response, size_t capacity);
}
