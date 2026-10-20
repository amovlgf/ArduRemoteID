/*
  WiFi beacon driver
 */
#pragma once

#include "transmitter.h"

class WiFi_TX : public Transmitter {
public:
    bool init(void) override;
    bool transmit_nan(ODID_UAS_Data &UAS_data);
    bool transmit_beacon(ODID_UAS_Data &UAS_data);
    bool transmit_cn(const uint8_t *payload, size_t length, const uint8_t identifier[4]);
    bool clear_cn(void);
    bool management_ready() const { return ap_ready; }

private:
    bool initialised;
    bool ap_ready = false;
    bool mac_ready = false;
    bool init_attempted = false;
    uint32_t last_init_attempt_ms = 0;
    bool cn_beacon_active = false;
    bool cn_probe_active = false;
    char ssid[32];
    uint8_t WiFi_mac_addr[6];
    size_t ssid_length;
    uint8_t send_counter_nan;
    uint8_t send_counter_beacon;
    uint8_t dBm_to_tx_power(float dBm) const;
};
