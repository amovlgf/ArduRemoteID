#pragma once
#include "esp_system.h"

struct esp_netif_t {};
struct esp_ip4_addr_t { uint32_t addr = 0; };
struct esp_netif_ip_info_t { esp_ip4_addr_t ip, netmask, gw; };
enum esp_netif_dhcp_status_t { ESP_NETIF_DHCP_INIT, ESP_NETIF_DHCP_STARTED, ESP_NETIF_DHCP_STOPPED };

esp_netif_t *esp_netif_get_handle_from_ifkey(const char *key);
bool esp_netif_is_netif_up(esp_netif_t *netif);
esp_err_t esp_netif_get_ip_info(esp_netif_t *netif, esp_netif_ip_info_t *info);
esp_err_t esp_netif_dhcps_get_status(esp_netif_t *netif, esp_netif_dhcp_status_t *status);
