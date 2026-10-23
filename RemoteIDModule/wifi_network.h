#pragma once

#include <WiFi.h>
#include <esp_netif.h>

// softAP() only configures the radio. The AP event, IP interface and DHCP
// server become ready separately, and can also stop after a successful boot.
namespace WiFiNetwork {

inline esp_netif_t *ap_netif()
{
    return esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
}

inline bool ap_up()
{
    auto *netif = ap_netif();
    return (WiFi.getStatusBits() & AP_STARTED_BIT) != 0 &&
           netif != nullptr && esp_netif_is_netif_up(netif);
}

inline bool address_ready()
{
    auto *netif = ap_netif();
    esp_netif_ip_info_t info {};
    return netif != nullptr && esp_netif_get_ip_info(netif, &info) == ESP_OK &&
           IPAddress(info.ip.addr) == IPAddress(192, 168, 4, 1) &&
           IPAddress(info.gw.addr) == IPAddress(192, 168, 4, 1) &&
           IPAddress(info.netmask.addr) == IPAddress(255, 255, 255, 0);
}

inline bool ready()
{
    auto *netif = ap_netif();
    esp_netif_dhcp_status_t status = ESP_NETIF_DHCP_INIT;
    return ap_up() && address_ready() &&
           esp_netif_dhcps_get_status(netif, &status) == ESP_OK &&
           status == ESP_NETIF_DHCP_STARTED;
}

} // namespace WiFiNetwork
