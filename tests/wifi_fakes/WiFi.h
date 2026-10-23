#pragma once
#include "Arduino.h"
#include "esp_wifi.h"

constexpr uint32_t AP_STARTED_BIT = 1U << 0;
class WiFiClass {
public:
    bool softAP(const char *ssid, const char *password, int channel = 1,
                int hidden = 0, int max_connection = 4, bool ftm_responder = false);
    bool softAPConfig(IPAddress local, IPAddress gateway, IPAddress subnet);
    bool mode(wifi_mode_t mode);
    wifi_mode_t getMode();
    int getStatusBits();
    IPAddress softAPIP();
};
extern WiFiClass WiFi;
