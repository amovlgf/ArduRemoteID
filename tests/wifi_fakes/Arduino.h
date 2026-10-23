#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>

uint32_t millis();
long random(long limit);
struct FakeSerial {
    template<typename... Args> void printf(const char *, Args...) {}
};
extern FakeSerial Serial;

class IPAddress {
public:
    IPAddress() = default;
    explicit IPAddress(uint32_t value) : value_(value) {}
    IPAddress(uint8_t a, uint8_t b, uint8_t c, uint8_t d)
        : value_(uint32_t(a) | (uint32_t(b) << 8) | (uint32_t(c) << 16) | (uint32_t(d) << 24)) {}
    operator uint32_t() const { return value_; }
    bool operator==(const IPAddress &other) const { return value_ == other.value_; }
    bool operator!=(const IPAddress &other) const { return !(*this == other); }
private:
    uint32_t value_ = 0;
};
