#pragma once
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>
class String {};
class Stream {
public:
    virtual ~Stream()=default;
    virtual size_t write(const uint8_t *,size_t)=0;
    virtual size_t write(uint8_t)=0;
    virtual int available()=0;
    virtual int read()=0;
    virtual int peek()=0;
    virtual void flush()=0;
    virtual size_t readBytes(char *,size_t)=0;
};
struct FakeSerial { template<typename... Args> void printf(const char *,Args...) {} };
extern FakeSerial Serial;
uint32_t millis();
void esp_restart();
constexpr int ESP_MAC_WIFI_STA=0;
void esp_read_mac(uint8_t *,int);
