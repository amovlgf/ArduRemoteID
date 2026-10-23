#include "RemoteIDModule/WiFi_TX.h"
#include "RemoteIDModule/parameters.h"
#include "RemoteIDModule/wifi_network.h"
#include <algorithm>
#include <array>
#include <cassert>
#include <iostream>
#include <iterator>
#include <limits>
#include <string>
#include <vector>

// Only the SDK boundary and packet encoder are faked. AP recovery, network
// readiness, retry timing and MAC generation run the actual firmware code.
namespace {
struct State {
    uint32_t now = 100;
    wifi_mode_t mode = WIFI_OFF;
    uint32_t status_bits = 0;
    bool have_netif = true;
    bool netif_up = false;
    bool start_succeeds = true;
    bool emit_ap_start = true;
    bool bring_netif_up = true;
    bool stop_succeeds = true;
    bool config_succeeds = true;
    bool config_starts_dhcp = true;
    bool ip_query_succeeds = true;
    bool dhcp_query_succeeds = true;
    bool mac_succeeds = true;
    bool bandwidth_succeeds = true;
    bool power_succeeds = true;
    bool remove_ie_succeeds = true;
    esp_netif_dhcp_status_t dhcp = ESP_NETIF_DHCP_INIT;
    esp_netif_ip_info_t ip {};
    unsigned starts = 0, stops = 0, configs = 0, mac_sets = 0, random_calls = 0;
    unsigned bandwidth_sets = 0, power_sets = 0, tx_calls = 0, ie_removes = 0;
    int connection_limit = -1;
    uint8_t channel = 0;
    int8_t power = 0;
    std::string ssid, password;
    std::array<uint8_t, 6> mac {};
    std::vector<std::string> operations;
} state;
esp_netif_t ap;

void reset()
{
    state = State{};
    g = Parameters{};
    g.webserver_enable = 1;
    g.wifi_power = 17;
    g.wifi_beacon_rate = 1;
}

void advance(uint32_t elapsed = 1000) { state.now += elapsed; }

void assert_address()
{
    assert(state.ip.ip.addr == uint32_t(IPAddress(192, 168, 4, 1)));
    assert(state.ip.gw.addr == uint32_t(IPAddress(192, 168, 4, 1)));
    assert(state.ip.netmask.addr == uint32_t(IPAddress(255, 255, 255, 0)));
}
} // namespace

Parameters g;
bool cn_mode_active = false;
WiFiClass WiFi;
FakeSerial Serial;

uint32_t millis() { return state.now; }
long random(long limit) { return (++state.random_calls * 37U) % limit; }
// The base virtual has no production implementation (all transports override
// it); supplying it lets the host toolchain emit normal RTTI and vtables.
bool Transmitter::init() { assert(false); return false; }

bool WiFiClass::softAP(const char *ssid, const char *password, int channel, int hidden,
                       int max_connection, bool)
{
    ++state.starts;
    state.operations.push_back("start");
    assert(hidden == 0);
    state.ssid = ssid;
    state.password = password;
    state.channel = channel;
    state.connection_limit = max_connection;
    if (!state.start_succeeds) return false;
    state.mode = WIFI_AP;
    state.status_bits = state.emit_ap_start ? AP_STARTED_BIT : 0;
    state.netif_up = state.bring_netif_up;
    return true;
}

bool WiFiClass::softAPConfig(IPAddress local, IPAddress gateway, IPAddress subnet)
{
    ++state.configs;
    state.operations.push_back("config");
    // Catch configuration racing the asynchronous AP_START/netif transition.
    assert(state.mode & WIFI_AP);
    assert(state.status_bits & AP_STARTED_BIT);
    assert(state.have_netif && state.netif_up);
    if (!state.config_succeeds) return false;
    state.ip.ip.addr = local;
    state.ip.gw.addr = gateway;
    state.ip.netmask.addr = subnet;
    if (state.config_starts_dhcp) state.dhcp = ESP_NETIF_DHCP_STARTED;
    return true;
}

bool WiFiClass::mode(wifi_mode_t mode)
{
    assert(mode == WIFI_OFF);
    ++state.stops;
    state.operations.push_back("stop");
    if (!state.stop_succeeds) return false;
    state.mode = WIFI_OFF;
    state.status_bits = 0;
    state.netif_up = false;
    state.dhcp = ESP_NETIF_DHCP_STOPPED;
    return true;
}

wifi_mode_t WiFiClass::getMode() { return state.mode; }
int WiFiClass::getStatusBits() { return state.status_bits; }
IPAddress WiFiClass::softAPIP() { return IPAddress(state.ip.ip.addr); }

esp_netif_t *esp_netif_get_handle_from_ifkey(const char *key)
{
    assert(std::string(key) == "WIFI_AP_DEF");
    return state.have_netif ? &ap : nullptr;
}
bool esp_netif_is_netif_up(esp_netif_t *netif) { assert(netif == &ap); return state.netif_up; }
esp_err_t esp_netif_get_ip_info(esp_netif_t *netif, esp_netif_ip_info_t *info)
{
    assert(netif == &ap);
    if (!state.ip_query_succeeds) return ESP_FAIL;
    *info = state.ip;
    return ESP_OK;
}
esp_err_t esp_netif_dhcps_get_status(esp_netif_t *netif, esp_netif_dhcp_status_t *status)
{
    assert(netif == &ap);
    if (!state.dhcp_query_succeeds) return ESP_FAIL;
    *status = state.dhcp;
    return ESP_OK;
}
esp_err_t esp_base_mac_addr_set(const uint8_t *mac)
{
    ++state.mac_sets;
    std::copy(mac, mac + 6, state.mac.begin());
    return state.mac_succeeds ? ESP_OK : ESP_FAIL;
}
esp_err_t esp_wifi_set_bandwidth(wifi_interface_t interface, wifi_bandwidth_t bandwidth)
{
    assert(interface == WIFI_IF_AP && bandwidth == WIFI_BW_HT20);
    ++state.bandwidth_sets;
    return state.bandwidth_succeeds ? ESP_OK : ESP_FAIL;
}
esp_err_t esp_wifi_set_max_tx_power(int8_t power)
{
    ++state.power_sets;
    state.power = power;
    return state.power_succeeds ? ESP_OK : ESP_FAIL;
}
esp_err_t esp_wifi_80211_tx(wifi_interface_t interface, const void *, int length, bool)
{
    assert(interface == WIFI_IF_AP && length > 0);
    ++state.tx_calls;
    return ESP_OK;
}
esp_err_t esp_wifi_set_vendor_ie(bool enable, wifi_vendor_ie_type_t, wifi_vendor_ie_id_t, const void *)
{
    if (enable) return ESP_OK;
    ++state.ie_removes;
    return state.remove_ie_succeeds ? ESP_OK : ESP_FAIL;
}

int odid_wifi_build_nan_sync_beacon_frame(char *, uint8_t *, size_t) { return 24; }
int odid_wifi_build_message_pack_nan_action_frame(ODID_UAS_Data *, char *, uint8_t, uint8_t *, size_t) { return 24; }
int odid_wifi_build_message_pack_beacon_frame(ODID_UAS_Data *, char *, const char *, size_t,
                                            uint16_t, uint8_t, uint8_t *, size_t) { return 64; }

static void test_start_retry_preserves_mac()
{
    reset();
    WiFi_TX driver {};
    state.start_succeeds = false;
    assert(!driver.init() && !driver.management_ready());
    assert(state.starts == 1 && state.configs == 0);
    const auto mac = state.mac;
    assert((mac[0] & 3) == 2); // Locally administered unicast.
    state.start_succeeds = true;
    advance(999);
    assert(!driver.init() && state.starts == 1);
    advance(1);
    assert(driver.init() && driver.management_ready());
    assert(state.starts == 2 && state.configs == 1);
    assert(state.mac == mac && state.mac_sets == 1 && state.random_calls == 6);
    assert(state.ssid == g.wifi_ssid && state.password == g.wifi_password);
    assert(state.channel == g.wifi_channel && state.connection_limit == 4);
    assert_address();
}

static void test_async_start_and_grace()
{
    reset();
    WiFi_TX driver {};
    state.emit_ap_start = false;
    state.bring_netif_up = false;
    assert(!driver.init() && !driver.management_ready());
    advance();
    state.status_bits = AP_STARTED_BIT;
    assert(!driver.init() && state.configs == 0);
    advance();
    state.netif_up = true;
    assert(driver.init() && driver.management_ready());
    assert(state.starts == 1 && state.stops == 0 && state.configs == 1);

    // An AP mode bit alone must not keep a wedged startup alive indefinitely.
    for (bool missing_event : {false, true}) {
        reset();
        WiFi_TX wedged {};
        state.emit_ap_start = !missing_event;
        state.bring_netif_up = missing_event;
        assert(!wedged.init());
        for (unsigned i = 0; i < 4; ++i) {
            advance();
            assert(!wedged.init());
            assert(state.stops == 0 && state.starts == 1 && state.configs == 0);
        }
        advance(999);
        assert(!wedged.init() && state.stops == 0);
        advance(1);
        assert(!wedged.init() && state.stops == 1 && !wedged.management_ready());
        state.emit_ap_start = state.bring_netif_up = true;
        advance(999);
        assert(!wedged.init() && state.starts == 1);
        advance(1);
        assert(wedged.init() && wedged.management_ready());
        assert(state.starts == 2 && state.configs == 1 && state.mac_sets == 1);
    }
}

static void test_healthy_ap_is_not_restarted()
{
    reset();
    WiFi_TX driver {};
    assert(driver.init() && driver.management_ready());
    for (unsigned i = 0; i < 100; ++i) {
        advance(300);
        assert(driver.init() && driver.management_ready());
    }
    assert(state.starts == 1 && state.configs == 1 && state.stops == 0);
    assert(state.bandwidth_sets == 1 && state.power_sets == 1);
}

static void test_dhcp_recovery_keeps_radio_and_clients()
{
    reset();
    WiFi_TX driver {};
    state.config_succeeds = false;
    assert(driver.init() && !driver.management_ready());
    assert(state.configs == 1);
    state.config_succeeds = true;
    advance(999);
    driver.init();
    assert(state.configs == 1);
    advance(1);
    assert(driver.init() && driver.management_ready());
    assert_address();
    state.dhcp = ESP_NETIF_DHCP_STOPPED;
    state.config_starts_dhcp = false;
    advance();
    assert(driver.init() && !driver.management_ready());
    assert(state.configs == 3);
    state.config_starts_dhcp = true;
    advance();
    assert(driver.init() && driver.management_ready() && state.configs == 4);
    assert(state.starts == 1 && state.stops == 0 && state.mac_sets == 1);
}

static void test_readiness_checks_live_network()
{
    reset();
    WiFi_TX driver {};
    assert(driver.init() && WiFiNetwork::ready());
    state.have_netif = false;
    assert(!WiFiNetwork::ready());
    state.have_netif = true;
    state.netif_up = false;
    assert(!WiFiNetwork::ready());
    state.netif_up = true;
    state.status_bits = 0;
    assert(!WiFiNetwork::ready());
    state.status_bits = AP_STARTED_BIT;
    state.ip_query_succeeds = false;
    assert(!WiFiNetwork::ready());
    state.ip_query_succeeds = true;
    const auto address = state.ip;
    for (auto *field : {&state.ip.ip, &state.ip.gw, &state.ip.netmask}) {
        field->addr = 0;
        assert(!WiFiNetwork::ready());
        state.ip = address;
    }
    state.dhcp_query_succeeds = false;
    assert(!WiFiNetwork::ready());
    state.dhcp_query_succeeds = true;
    for (const auto status : {ESP_NETIF_DHCP_INIT, ESP_NETIF_DHCP_STOPPED}) {
        state.dhcp = status;
        assert(!WiFiNetwork::ready());
    }
    state.dhcp = ESP_NETIF_DHCP_STARTED;
    assert(WiFiNetwork::ready());

    // A wrong address is repaired without replacing the radio connection.
    state.ip.ip.addr = uint32_t(IPAddress(192, 168, 7, 1));
    advance();
    assert(driver.init() && driver.management_ready());
    assert_address();
    assert(state.configs == 2 && state.starts == 1 && state.stops == 0);
}

static void test_radio_failure_does_not_hide_management()
{
    for (bool fail_bandwidth : {false, true}) {
        reset();
        WiFi_TX driver {};
        state.bandwidth_succeeds = !fail_bandwidth;
        state.power_succeeds = fail_bandwidth;
        assert(!driver.init() && driver.management_ready());
        assert_address();
        advance();
        assert(!driver.init() && driver.management_ready());
        state.bandwidth_succeeds = state.power_succeeds = true;
        advance();
        assert(driver.init() && driver.management_ready());
        assert(state.starts == 1 && state.configs == 1 && state.stops == 0);
    }
}

static void test_ap_mode_loss_recovers()
{
    reset();
    WiFi_TX driver {};
    assert(driver.init());
    const auto mac = state.mac;
    state.mode = WIFI_OFF;
    state.status_bits = 0;
    state.netif_up = false;
    state.dhcp = ESP_NETIF_DHCP_STOPPED;
    advance();
    assert(driver.init() && driver.management_ready());
    assert(state.starts == 2 && state.configs == 2);
    assert(state.mac == mac && state.mac_sets == 1 && state.random_calls == 6);
}

static void test_runtime_interface_loss()
{
    for (bool missing_event : {false, true}) {
        reset();
        WiFi_TX driver {};
        assert(driver.init() && driver.management_ready());
        // Startup is complete; losing an interface later invalidates cached
        // readiness, even though the SDK still reports AP mode enabled.
        advance(6000);
        if (missing_event) state.status_bits = 0;
        else state.netif_up = false;
        assert(!driver.init() && !driver.management_ready());
        assert(state.stops == 1 && state.starts == 1);
        ODID_UAS_Data uas {};
        advance(999);
        assert(!driver.transmit_nan(uas) && state.tx_calls == 0);
        advance(1);
        assert(driver.init() && driver.management_ready());
        assert(state.starts == 2 && state.mac_sets == 1);
    }
}

static void test_connection_limits()
{
    for (unsigned web = 0; web <= 1; ++web) {
        for (unsigned cn = 0; cn <= 1; ++cn) {
            reset();
            g.webserver_enable = web;
            cn_mode_active = cn;
            WiFi_TX driver {};
            assert(driver.init());
            assert(state.connection_limit == ((web || cn) ? 4 : 0));
        }
    }
    cn_mode_active = false;
}

static void test_cn_failure_retries_shutdown_before_restart()
{
    reset();
    WiFi_TX driver {};
    const uint8_t payload[6] {}, identifier[4] {0xfa, 0x0b, 0xbc, 0x0d};
    assert(driver.transmit_cn(payload, sizeof(payload), identifier));
    state.remove_ie_succeeds = state.stop_succeeds = false;
    assert(!driver.clear_cn() && !driver.management_ready());
    assert(state.stops == 1 && state.starts == 1);
    ODID_UAS_Data uas {};
    advance(999);
    assert(!driver.transmit_nan(uas) && state.tx_calls == 0);
    assert(state.stops == 1 && state.starts == 1);
    advance(1);
    assert(!driver.init() && state.stops == 2 && state.starts == 1);
    assert(!driver.management_ready());
    state.stop_succeeds = true;
    advance();
    assert(driver.init() && driver.management_ready());
    assert(state.stops == 3 && state.starts == 2 && state.mac_sets == 1);
    const auto last_start = std::find(state.operations.rbegin(), state.operations.rend(), "start");
    assert(last_start != state.operations.rend() && *std::next(last_start) == "stop");
    assert(driver.clear_cn()); // A successful stop discarded both stale IEs.
}

static void test_retry_clock_wrap()
{
    reset();
    WiFi_TX driver {};
    state.now = std::numeric_limits<uint32_t>::max() - 500;
    state.start_succeeds = false;
    assert(!driver.init());
    advance(999);
    assert(!driver.init() && state.starts == 1);
    state.start_succeeds = true;
    advance(1);
    assert(driver.init() && driver.management_ready() && state.starts == 2);
}

int main()
{
    test_start_retry_preserves_mac();
    test_async_start_and_grace();
    test_healthy_ap_is_not_restarted();
    test_dhcp_recovery_keeps_radio_and_clients();
    test_readiness_checks_live_network();
    test_radio_failure_does_not_hide_management();
    test_ap_mode_loss_recovers();
    test_runtime_interface_loss();
    test_connection_limits();
    test_cn_failure_retries_shutdown_before_restart();
    test_retry_clock_wrap();
    std::cout << "WiFi: real driver AP/DHCP fault recovery, readiness, retry bounds, radio failures, "
                 "connection limits, MAC stability and CN shutdown recovery passed\n";
}
