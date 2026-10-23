#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace ParameterText {
// Wire strings use explicit lengths. Validate before copying into NUL-terminated
// parameter storage; embedded NUL must never turn an invalid value into a prefix.
inline bool valid(const uint8_t *bytes, size_t length, size_t maximum) {
    return length <= maximum && (!length || (bytes && !memchr(bytes, 0, length)));
}
}
