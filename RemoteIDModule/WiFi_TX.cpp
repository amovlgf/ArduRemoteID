/*
  WiFi NAN and Beacon driver

  with thanks to https://github.com/sxjack/uav_electronic_ids for WiFi calls
 */

#include "WiFi_TX.h"
#include <esp_wifi.h>
#include <WiFi.h>
#include <esp_system.h>
#include "parameters.h"
#include "CNData.h"
#include <math.h>

bool WiFi_TX::init(void)
{
    if (initialised) {
        return true;
    }
    const uint32_t now = millis();
    if (init_attempted && now - last_init_attempt_ms < 1000) return false;
    init_attempted = true;
    last_init_attempt_ms = now;

    //use a local MAC address to avoid tracking transponders based on their MAC address
    if (!mac_ready) {
        generate_random_mac(WiFi_mac_addr);
        WiFi_mac_addr[0] |= 0x02;
        WiFi_mac_addr[0] &= 0xFE;
        if (esp_base_mac_addr_set(WiFi_mac_addr) != ESP_OK) return false;
        mac_ready = true;
    }
    if (!ap_ready) {
        // CN faults must not remove the field-update path. Keep stored settings.
        ap_ready = WiFi.softAP(g.wifi_ssid, g.wifi_password, g.wifi_channel,
                              false, (g.webserver_enable || cn_mode_active) ? 1 : 0);
        if (!ap_ready) return false;
    }
    if (esp_wifi_set_bandwidth(WIFI_IF_AP, WIFI_BW_HT20) != ESP_OK) {
        return false;
    }

    if (esp_wifi_set_max_tx_power(dBm_to_tx_power(g.wifi_power)) != ESP_OK) return false;
    initialised = true;
    return true;
}

bool WiFi_TX::transmit_nan(ODID_UAS_Data &UAS_data)
{
    if (!init()) return false;

    uint8_t buffer[1024] {};

    int length;
    if ((length = odid_wifi_build_nan_sync_beacon_frame((char *)WiFi_mac_addr,
                  buffer,sizeof(buffer))) > 0) {
        if (esp_wifi_80211_tx(WIFI_IF_AP,buffer,length,true) != ESP_OK) {
            return false;
        }
    }

    if ((length = odid_wifi_build_message_pack_nan_action_frame(&UAS_data,(char *)WiFi_mac_addr,
                  ++send_counter_nan,
                  buffer,sizeof(buffer))) > 0) {
        if (esp_wifi_80211_tx(WIFI_IF_AP,buffer,length,true) != ESP_OK) {
            return false;
        }
    }

    return true;
}

//update the payload of the beacon frames in this function
bool WiFi_TX::transmit_beacon(ODID_UAS_Data &UAS_data)
{
    if (!init()) return false;

    uint8_t buffer[1024] {};

    int length;
    if ((length = odid_wifi_build_message_pack_beacon_frame(&UAS_data,(char *)WiFi_mac_addr,
                   "UAS_ID_OPEN", strlen("UAS_ID_OPEN"), //use dummy SSID, as we only extract payload data
                  1000/g.wifi_beacon_rate, ++send_counter_beacon, buffer, sizeof(buffer))) > 0) {

        //set the RID IE element
        uint8_t header_offset = 58;
        // vendor_ie_data_t ends in a zero-length array; reserve payload storage.
        uint8_t ie_buffer[257] {};
        if (length < header_offset || length - header_offset > 251) return false;
        auto &IE_data = *reinterpret_cast<vendor_ie_data_t *>(ie_buffer);
        IE_data.element_id = WIFI_VENDOR_IE_ELEMENT_ID;
        IE_data.vendor_oui[0] = 0xFA;
        IE_data.vendor_oui[1] = 0x0B;
        IE_data.vendor_oui[2] = 0xBC;
        IE_data.vendor_oui_type = 0x0D;
        IE_data.length = length - header_offset + 4; //add 4 as of definition esp_wifi_set_vendor_ie
        memcpy(IE_data.payload,&buffer[header_offset],length - header_offset);

        // so first remove old element, add new afterwards
        if (esp_wifi_set_vendor_ie(false, WIFI_VND_IE_TYPE_BEACON, WIFI_VND_IE_ID_0, &IE_data) != ESP_OK){
            return false;
        }

        if (esp_wifi_set_vendor_ie(true, WIFI_VND_IE_TYPE_BEACON, WIFI_VND_IE_ID_0, &IE_data) != ESP_OK){
            return false;
        }

        //set the payload also to probe requests, to increase update rate on mobile phones
        // so first remove old element, add new afterwards
        if (esp_wifi_set_vendor_ie(false, WIFI_VND_IE_TYPE_PROBE_RESP, WIFI_VND_IE_ID_0, &IE_data) != ESP_OK){
            return false;
        }

        if (esp_wifi_set_vendor_ie(true, WIFI_VND_IE_TYPE_PROBE_RESP, WIFI_VND_IE_ID_0, &IE_data) != ESP_OK){
            return false;
        }

        return true;
	}
	else {
		return false;
	}
}

bool WiFi_TX::transmit_cn(const uint8_t *payload, size_t length, const uint8_t identifier[4])
{
    if (!init() || payload == nullptr || identifier == nullptr || length < 6 || length > 251) return false;
    uint8_t buffer[257] {};
    auto *ie = reinterpret_cast<vendor_ie_data_t *>(buffer);
    ie->element_id = WIFI_VENDOR_IE_ELEMENT_ID;
    ie->length = uint8_t(length + 4);
    memcpy(ie->vendor_oui, identifier, 3);
    ie->vendor_oui_type = identifier[3];
    memcpy(ie->payload, payload, length);
    if (!clear_cn()) return false;
    cn_beacon_active = true;
    if (esp_wifi_set_vendor_ie(true, WIFI_VND_IE_TYPE_BEACON, WIFI_VND_IE_ID_0, ie) != ESP_OK) {
        clear_cn();
        return false;
    }
    cn_probe_active = true;
    if (esp_wifi_set_vendor_ie(true, WIFI_VND_IE_TYPE_PROBE_RESP, WIFI_VND_IE_ID_0, ie) != ESP_OK) {
        clear_cn();
        return false;
    }
    return true;
}

bool WiFi_TX::clear_cn(void)
{
    bool ok = true;
    if (cn_beacon_active) {
        if (esp_wifi_set_vendor_ie(false, WIFI_VND_IE_TYPE_BEACON, WIFI_VND_IE_ID_0, nullptr) == ESP_OK) cn_beacon_active = false;
        else ok = false;
    }
    if (cn_probe_active) {
        if (esp_wifi_set_vendor_ie(false, WIFI_VND_IE_TYPE_PROBE_RESP, WIFI_VND_IE_ID_0, nullptr) == ESP_OK) cn_probe_active = false;
        else ok = false;
    }
    if (!ok) {
        // A stale normal beacon is worse than an explicit radio outage. Reset
        // the driver, then retry the same management AP after a bounded delay.
        if (WiFi.mode(WIFI_OFF)) {
            cn_beacon_active = false;
            cn_probe_active = false;
        }
        initialised = false;
        ap_ready = false;
        init_attempted = true;
        last_init_attempt_ms = millis();
    }
    return ok;
}

/*
  map dBm to a TX power
 */
uint8_t WiFi_TX::dBm_to_tx_power(float dBm) const
{
    if (!isfinite(dBm)) dBm = 2;
    if (dBm < 2) {
        dBm = 2;
    }
    if (dBm > 20) {
        dBm = 20;
    }
    return uint8_t((dBm+1.125) * 4);
}
