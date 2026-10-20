#include "CNData.h"
#include <math.h>
#include <string.h>

namespace {
constexpr uint32_t MAX_AGE_MS = 1000;
constexpr uint64_t EPOCH_2019_MS = 1546300800000ULL;
constexpr int64_t HOUR_MS = 3600000;

bool valid_position(int32_t lat, int32_t lon)
{
    // Existing DroneCAN ODID defines the pair (0, 0) as unavailable.
    return lat >= -900000000 && lat <= 900000000 &&
           lon >= -1800000000 && lon <= 1800000000 && (lat != 0 || lon != 0) &&
           lat != -1 && lon != -1; // GB reserves wire 0xffffffff for unavailable.
}

bool valid_altitude(float alt)
{
    return isfinite(alt) && alt >= -999.5f && alt <= 31767.5f;
}

bool valid_location_time(float t)
{
    return isfinite(t) && t >= 0.0f && t < 3600.0f;
}

bool ascii_length(const char *s, size_t required, bool empty_allowed = false)
{
    if (s == nullptr) {
        return false;
    }
    const size_t n = strnlen(s, 21);
    if (n == 0 && empty_allowed) {
        return true;
    }
    if (n != required) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        if (uint8_t(s[i]) < 33 || uint8_t(s[i]) > 126) {
            return false;
        }
    }
    return true;
}

int hex_digit(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
}

bool CNData::parse_wifi_id(const char *text, uint8_t out[4])
{
    if (text == nullptr || strnlen(text, 21) != 8) return false;
    for (uint8_t i = 0; i < 4; i++) {
        const int hi = hex_digit(text[2*i]);
        const int lo = hex_digit(text[2*i+1]);
        if (hi < 0 || lo < 0) return false;
        out[i] = uint8_t(hi * 16 + lo);
    }
    // Do not advertise a Chinese packet under the existing ASTM identifier.
    return !(out[0] == 0xFA && out[1] == 0x0B && out[2] == 0xBC && out[3] == 0x0D);
}

bool CNData::valid_config(const CNConfig &config)
{
    uint8_t id[4];
    return config.source_node >= 1 && config.source_node <= 127 &&
           config.aircraft_class <= 4 && ascii_length(config.uas_id, 20) &&
           ascii_length(config.uin, 8, true) && parse_wifi_id(config.wifi_id, id);
}

void CNData::select_source(uint8_t node)
{
    if (source != node) {
        *this = CNData{};
        source = node;
    }
}

void CNData::receive_location(uint8_t node, const dronecan_remoteid_Location &msg, uint32_t now)
{
    if (source == 0 || node != source) return;
    const bool valid_time = valid_location_time(msg.timestamp);
    if (have_location && location_time_valid && valid_time) {
        float delta = msg.timestamp - location.timestamp;
        if (delta < 0) delta += 3600.0f;
        // A duplicate cannot keep a frozen sample alive. Ignore out-of-order
        // packets; after an outage allow a new stream/time anchor to recover.
        if (delta > 1800.0f && now - location_sample_ms <= MAX_AGE_MS) return;
        if (delta == 0.0f) {
            location.status = msg.status;
            return;
        }
    }
    location = msg;
    have_location = true;
    location_time_valid = valid_time;
    location_sample_ms = now;
}

void CNData::receive_system(uint8_t node, const dronecan_remoteid_System &msg, uint32_t now)
{
    if (source == 0 || node != source) return;
    if (have_system && msg.timestamp < system.timestamp &&
        now - system_sample_ms <= 2000 && msg.timestamp != 0) return;
    if (!have_system || msg.timestamp != system.timestamp) system_sample_ms = now;
    system = msg;
    have_system = true;
    system_receive_ms = now;
}

bool CNData::prepare(const CNConfig &config, uint32_t now, DroneRIDData_t &out)
{
    select_source(config.source_node);
    out = DroneRIDData_t{};
    out.unknown_fields = CN46750_FIELD_UNKNOWN(6) | CN46750_FIELD_UNKNOWN(7) |
                         CN46750_FIELD_UNKNOWN(8) | CN46750_FIELD_UNKNOWN(9) |
                         CN46750_FIELD_UNKNOWN(10) | CN46750_FIELD_UNKNOWN(13) |
                         CN46750_FIELD_UNKNOWN(20);
    if (!valid_config(config)) {
        health_reason = "CN configuration required";
        return false;
    }
    memcpy(out.sn, config.uas_id, 20);
    memcpy(out.uin, config.uin, strlen(config.uin));
    out.drone_class = DroneClass_t(config.aircraft_class);
    out.coord_system = COORD_WGS84; // Existing DroneCAN fields specify WGS84.
    out.gcs_pos_type = POS_TYPE_TAKEOFF;
    const bool loc_fresh = have_location && location_time_valid && now - location_sample_ms <= MAX_AGE_MS;
    // System UTC has whole-second resolution: the value may repeat at 2 Hz.
    const bool sys_fresh = have_system && system.timestamp != 0 &&
                           now - system_receive_ms <= MAX_AGE_MS && now - system_sample_ms <= 2000;
    health_reason = nullptr;
    if (!loc_fresh) health_reason = "CN location stale/unknown";
    else if (!sys_fresh) health_reason = "CN system stale/unknown";

    if (sys_fresh && system.operator_location_type <= 2) {
        // ODID distinguishes live and fixed GCS positions; GB combines both.
        out.gcs_pos_type = system.operator_location_type == 0 ? POS_TYPE_TAKEOFF : POS_TYPE_GCS;
        if (valid_position(system.operator_latitude, system.operator_longitude)) {
            out.gcs_lat = system.operator_latitude * 1.0e-7;
            out.gcs_lon = system.operator_longitude * 1.0e-7;
            out.unknown_fields &= ~CN46750_FIELD_UNKNOWN(6);
        }
        if (valid_altitude(system.operator_altitude_geo)) {
            out.gcs_alt = system.operator_altitude_geo;
            out.unknown_fields &= ~CN46750_FIELD_UNKNOWN(7);
        }
    }
    if (loc_fresh) {
        if (valid_position(location.latitude, location.longitude)) {
            out.drone_lat = location.latitude * 1.0e-7;
            out.drone_lon = location.longitude * 1.0e-7;
            out.unknown_fields &= ~CN46750_FIELD_UNKNOWN(8);
            // ODID's 3.704 km and 185.2 m limits are wider than GB's limits.
            static const uint8_t hacc[] = {0, 1, 2, 2, 4, 5, 6, 6, 8, 9, 10, 11, 12};
            if (location.horizontal_accuracy < sizeof(hacc)) out.horizontal_accuracy = hacc[location.horizontal_accuracy];
        }
        if (valid_altitude(location.altitude_geodetic)) {
            out.drone_alt = location.altitude_geodetic;
            out.unknown_fields &= ~CN46750_FIELD_UNKNOWN(13);
            if (location.vertical_accuracy <= 6) out.vertical_accuracy = location.vertical_accuracy;
        }
        if (location.direction <= 35999) {
            out.track_angle = (location.direction / 10) * 0.1f;
            out.unknown_fields &= ~CN46750_FIELD_UNKNOWN(9);
        }
        if (location.speed_horizontal <= 25425) {
            out.ground_speed = (location.speed_horizontal / 10) * 0.1f;
            out.unknown_fields &= ~CN46750_FIELD_UNKNOWN(10);
            if (location.speed_accuracy <= 4) out.speed_accuracy = location.speed_accuracy;
        }
        if (sys_fresh) {
            const int64_t anchor = EPOCH_2019_MS + uint64_t(system.timestamp) * 1000ULL;
            int64_t sample = (anchor / HOUR_MS) * HOUR_MS + llround(double(location.timestamp) * 1000.0);
            if (sample - anchor > HOUR_MS / 2) sample -= HOUR_MS;
            if (anchor - sample > HOUR_MS / 2) sample += HOUR_MS;
            // Same-source, contemporaneous System is only an hour/date anchor.
            // Never use it to replace the actual aircraft sample timestamp.
            if (sample >= anchor - 2000 && sample <= anchor + 2000) {
                out.timestamp_ms = uint64_t(sample);
                out.unknown_fields &= ~CN46750_FIELD_UNKNOWN(20);
            }
        }
    }
    // This CAN service does not establish end-to-end time accuracy. Do not
    // claim a measured synchronization bound from its enum alone.
    out.timestamp_accuracy = 0;
    if (health_reason == nullptr && out.unknown_fields != 0) health_reason = "CN required data unknown";
    if (health_reason == nullptr && location.status > 3) health_reason = "CN invalid flight status";
    const bool emergency = have_location && location.status == 3;
    out.op_status = health_reason != nullptr ? (emergency ? OP_STATUS_FAIL_EMERGENCY : OP_STATUS_FAIL_NORMAL) : OperationStatus_t(location.status);
    return true;
}
