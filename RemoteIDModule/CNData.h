/* DroneCAN-only GB 46750 data adapter. No hardware or wall-clock dependency. */
#pragma once

#include <stdint.h>
#include <dronecan.remoteid.Location.h>
#include <dronecan.remoteid.System.h>
#include <opendroneidcn.h>

struct CNConfig {
    const char *uas_id;
    const char *uin;
    const char *wifi_id;
    uint8_t aircraft_class;
    uint8_t source_node;
};

class CNData {
public:
    void select_source(uint8_t node);
    void receive_location(uint8_t node, const dronecan_remoteid_Location &msg, uint32_t now);
    void receive_system(uint8_t node, const dronecan_remoteid_System &msg, uint32_t now);
    // False means invalid static configuration: do not emit a fabricated identity.
    bool prepare(const CNConfig &config, uint32_t now, DroneRIDData_t &out);
    const char *reason() const { return health_reason; }
    static bool valid_config(const CNConfig &config);
    static bool parse_wifi_id(const char *text, uint8_t out[4]);

private:
    uint8_t source = 0;
    bool have_location = false;
    bool have_system = false;
    bool location_time_valid = false;
    uint32_t location_sample_ms = 0;
    uint32_t system_receive_ms = 0;
    uint32_t system_sample_ms = 0;
    dronecan_remoteid_Location location {};
    dronecan_remoteid_System system {};
    const char *health_reason = "CN waiting for CAN";
};

// Selected once at boot. The legacy transport/MAVLink state is never reused.
extern bool cn_mode_active;
extern const char *cn_status_reason;
extern DroneRIDData_t cn_broadcast_data;
