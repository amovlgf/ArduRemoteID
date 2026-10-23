#pragma once
#include "esp_system.h"
#include <cstdint>

enum wifi_mode_t { WIFI_MODE_NULL = 0, WIFI_MODE_STA = 1, WIFI_MODE_AP = 2, WIFI_MODE_APSTA = 3 };
constexpr wifi_mode_t WIFI_OFF = WIFI_MODE_NULL;
constexpr wifi_mode_t WIFI_AP = WIFI_MODE_AP;
constexpr wifi_mode_t WIFI_STA = WIFI_MODE_STA;
constexpr wifi_mode_t WIFI_AP_STA = WIFI_MODE_APSTA;
enum wifi_interface_t { WIFI_IF_STA, WIFI_IF_AP };
enum wifi_bandwidth_t { WIFI_BW_HT20, WIFI_BW_HT40 };
enum wifi_vendor_ie_type_t { WIFI_VND_IE_TYPE_BEACON, WIFI_VND_IE_TYPE_PROBE_RESP };
enum wifi_vendor_ie_id_t { WIFI_VND_IE_ID_0 };
constexpr uint8_t WIFI_VENDOR_IE_ELEMENT_ID = 0xdd;
struct vendor_ie_data_t {
    uint8_t element_id;
    uint8_t length;
    uint8_t vendor_oui[3];
    uint8_t vendor_oui_type;
    uint8_t payload[0];
};

esp_err_t esp_wifi_set_bandwidth(wifi_interface_t interface, wifi_bandwidth_t bandwidth);
esp_err_t esp_wifi_set_max_tx_power(int8_t power);
esp_err_t esp_wifi_80211_tx(wifi_interface_t interface, const void *data, int length, bool sequence);
esp_err_t esp_wifi_set_vendor_ie(bool enable, wifi_vendor_ie_type_t type, wifi_vendor_ie_id_t id, const void *data);
