/* Tests the real DroneCAN adapter with generated message types and the CN codec. */
#include "CNData.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <initializer_list>

#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); std::exit(1); } } while (0)
static const uint64_t EPOCH = UINT64_C(1546300800000);
static const uint32_t LOCATION_UNKNOWN = CN46750_UNKNOWN_UAV_POSITION | CN46750_UNKNOWN_UAV_ALTITUDE |
    CN46750_UNKNOWN_TRACK_ANGLE | CN46750_UNKNOWN_GROUND_SPEED | CN46750_UNKNOWN_TIMESTAMP;

static CNConfig config()
{
    return {"12345678901234567890", "ABCDEFGH", "12345678", 2, 42};
}
static dronecan_remoteid_Location location(float seconds = 100.25f)
{
    dronecan_remoteid_Location m {};
    m.timestamp = seconds; m.status = 2;
    m.latitude = 40000000; m.longitude = -30000000;
    m.altitude_geodetic = 200;
    m.direction = 12349; m.speed_horizontal = 1239;
    m.horizontal_accuracy = 9; m.vertical_accuracy = 4; m.speed_accuracy = 3;
    m.timestamp_accuracy = 1;
    return m;
}
static dronecan_remoteid_System system_message(uint32_t seconds = 3700)
{
    dronecan_remoteid_System m {};
    m.timestamp = seconds; m.operator_location_type = 1;
    m.operator_latitude = -20000000; m.operator_longitude = 10000000;
    m.operator_altitude_geo = 100;
    return m;
}
static DroneRIDData_t prepare(CNData &adapter, uint32_t now, CNConfig cfg = config())
{
    DroneRIDData_t out {};
    CHECK(adapter.prepare(cfg, now, out));
    uint8_t packet[128]; size_t len = 0;
    CHECK(CN46750_RID_Encode(&out, packet, sizeof(packet), &len) == RID_OK);
    CHECK(len == 72 && packet[2] == 66 && packet[3] == 0xdf && packet[4] == 0xe5 && packet[5] == 0xfe);
    return out;
}
static void feed(CNData &adapter, uint32_t now = 10, float loc_time = 100.25f, uint32_t sys_time = 3700)
{
    adapter.select_source(42);
    adapter.receive_location(42, location(loc_time), now);
    adapter.receive_system(42, system_message(sys_time), now);
}

static void configuration()
{
    CNConfig cfg = config(); CHECK(CNData::valid_config(cfg));
    cfg.uin = ""; CHECK(CNData::valid_config(cfg));
    CNData adapter; feed(adapter);
    DroneRIDData_t out = prepare(adapter, 10, cfg); CHECK(out.uin[0] == 0);
    const CNConfig bad[] = {
        {nullptr, "ABCDEFGH", "12345678", 2, 42},
        {"", "ABCDEFGH", "12345678", 2, 42},
        {"1234567890123456789", "ABCDEFGH", "12345678", 2, 42},
        {"12345678901234567890", "ABC", "12345678", 2, 42},
        {"12345678901234567890", "ABCDEFGH", "", 2, 42},
        {"12345678901234567890", "ABCDEFGH", "FA0BBC0D", 2, 42},
        {"12345678901234567890", "ABCDEFGH", "1234567Z", 2, 42},
        {"12345678901234567890", "ABCDEFGH", "12345678", 255, 42},
        {"12345678901234567890", "ABCDEFGH", "12345678", 2, 0},
        {"12345678901234567890", "ABCDEFGH", "12345678", 2, 128},
    };
    for (const auto &item : bad) {
        CHECK(!CNData::valid_config(item));
        CHECK(!adapter.prepare(item, 20, out));
        CHECK(adapter.reason() != nullptr);
        CHECK(out.unknown_fields & CN46750_UNKNOWN_UAV_POSITION);
        CHECK(out.unknown_fields & CN46750_UNKNOWN_TIMESTAMP);
    }
    uint8_t id[4]; CHECK(CNData::parse_wifi_id("aBcD1234", id));
    CHECK(id[0] == 0xab && id[1] == 0xcd && id[2] == 0x12 && id[3] == 0x34);
}

static void source_isolation()
{
    CNData a; a.select_source(42);
    a.receive_location(43, location(), 10); a.receive_system(43, system_message(), 10);
    auto out = prepare(a, 10); CHECK(out.op_status == OP_STATUS_FAIL_NORMAL);
    CHECK((out.unknown_fields & LOCATION_UNKNOWN) == LOCATION_UNKNOWN);
    a.receive_location(42, location(), 20);
    out = prepare(a, 20); CHECK(out.unknown_fields & CN46750_UNKNOWN_TIMESTAMP);
    CHECK(out.unknown_fields & CN46750_UNKNOWN_GCS_POSITION);
    a.receive_system(42, system_message(), 20);
    out = prepare(a, 20); CHECK(out.unknown_fields == 0);
    CHECK(out.timestamp_ms == EPOCH + UINT64_C(3700250));
    auto other = location(101); other.latitude = 123000000;
    a.receive_location(43, other, 30);
    out = prepare(a, 30); CHECK(out.drone_lat == 4 && out.timestamp_ms == EPOCH + UINT64_C(3700250));
    CNConfig cfg = config(); cfg.source_node = 43;
    out = prepare(a, 40, cfg); CHECK(out.op_status == OP_STATUS_FAIL_NORMAL);
    CHECK((out.unknown_fields & LOCATION_UNKNOWN) == LOCATION_UNKNOWN);
    a.receive_location(42, location(), 50); a.receive_system(42, system_message(), 50);
    out = prepare(a, 50, cfg); CHECK(out.op_status == OP_STATUS_FAIL_NORMAL);
    a.receive_location(43, location(), 60); a.receive_system(43, system_message(), 60);
    out = prepare(a, 60, cfg); CHECK(out.unknown_fields == 0);
}

static void first_zero_and_duplicates()
{
    CNData a; feed(a, 0, 0, 3600);
    auto out = prepare(a, 0); CHECK(out.unknown_fields == 0);
    CHECK(out.timestamp_ms == EPOCH + UINT64_C(3600000));
    auto duplicate = location(0); duplicate.status = 3; duplicate.latitude = 99000000;
    a.receive_location(42, duplicate, 500);
    a.receive_system(42, system_message(3600), 500);
    out = prepare(a, 500); CHECK(out.drone_lat == 4 && out.op_status == OP_STATUS_EMERGENCY);
    out = prepare(a, 1000); CHECK(!(out.unknown_fields & CN46750_UNKNOWN_UAV_POSITION));
    out = prepare(a, 1001); CHECK((out.unknown_fields & LOCATION_UNKNOWN) == LOCATION_UNKNOWN);
    CHECK(out.op_status == OP_STATUS_FAIL_EMERGENCY);
    // A duplicate never makes the stale sample fresh again.
    a.receive_location(42, duplicate, 1100);
    out = prepare(a, 1100); CHECK(out.unknown_fields & CN46750_UNKNOWN_UAV_POSITION);
    auto recovered = location(1.25f); recovered.status = 2;
    a.receive_location(42, recovered, 1250); a.receive_system(42, system_message(3601), 1250);
    out = prepare(a, 1250); CHECK(out.unknown_fields == 0 && out.op_status == OP_STATUS_AIR);
}

static void out_of_order_and_hour_wrap()
{
    CNData a; feed(a, 1000, 101, 3701);
    auto older = location(100.5f); older.latitude = 99000000;
    a.receive_location(42, older, 1100); a.receive_system(42, system_message(3700), 1100);
    auto out = prepare(a, 1100);
    CHECK(out.drone_lat == 4 && out.timestamp_ms == EPOCH + UINT64_C(3701000));
    // After an outage a restarted source can establish a fresh time anchor.
    a.receive_location(42, location(10), 4000); a.receive_system(42, system_message(3610), 4000);
    out = prepare(a, 4000); CHECK(out.unknown_fields == 0);
    CHECK(out.timestamp_ms == EPOCH + UINT64_C(3610000));
    CNData b; feed(b, 1000, 3599.75f, 3599);
    out = prepare(b, 1000); CHECK(out.timestamp_ms == EPOCH + UINT64_C(3599750));
    b.receive_location(42, location(0.25f), 1500); b.receive_system(42, system_message(3600), 1500);
    out = prepare(b, 1500); CHECK(out.unknown_fields == 0);
    CHECK(out.timestamp_ms == EPOCH + UINT64_C(3600250));
    // A still-current System anchor from the previous hour resolves the wrap.
    CNData c; feed(c, 1000, 0.25f, 3599);
    out = prepare(c, 1000); CHECK(out.timestamp_ms == EPOCH + UINT64_C(3600250));
}

static void clocks_and_staleness()
{
    CNData a; const uint32_t start = UINT32_MAX - 200; feed(a, start);
    auto out = prepare(a, 99); CHECK(out.unknown_fields == 0);
    out = prepare(a, 1100); CHECK(out.op_status == OP_STATUS_FAIL_NORMAL);
    CHECK((out.unknown_fields & LOCATION_UNKNOWN) == LOCATION_UNKNOWN);
    CNData b; feed(b, 0, 100, 3700);
    for (uint32_t now = 500; now <= 2500; now += 500) {
        b.receive_system(42, system_message(3700), now);
        b.receive_location(42, location(100 + float(now) / 1000), now);
    }
    out = prepare(b, 2500);
    CHECK(out.unknown_fields & CN46750_UNKNOWN_GCS_POSITION);
    CHECK(out.unknown_fields & CN46750_UNKNOWN_TIMESTAMP);
    CHECK(!(out.unknown_fields & CN46750_UNKNOWN_UAV_POSITION));
    CHECK(out.op_status == OP_STATUS_FAIL_NORMAL);
    CNData c; feed(c, 100, 100, 7250);
    out = prepare(c, 100); CHECK(out.unknown_fields == CN46750_UNKNOWN_TIMESTAMP);
    CHECK(out.op_status == OP_STATUS_FAIL_NORMAL);
    for (float invalid : {std::numeric_limits<float>::quiet_NaN(), 65535.0f, -1.0f, 3600.0f}) {
        CNData d; feed(d, 100, invalid, 3700);
        out = prepare(d, 100); CHECK((out.unknown_fields & LOCATION_UNKNOWN) == LOCATION_UNKNOWN);
    }
}

static void field_mapping()
{
    CNData a; feed(a);
    auto out = prepare(a, 10);
    CHECK(std::fabs(out.track_angle - 123.4f) < 0.0001f);
    CHECK(std::fabs(out.ground_speed - 12.3f) < 0.0001f);
    CHECK(out.timestamp_accuracy == 0); // No unsupported synchronization claim.
    auto loc = location(100.5f); loc.horizontal_accuracy = 3; loc.vertical_accuracy = 15; loc.speed_accuracy = 15;
    a.receive_location(42, loc, 20); out = prepare(a, 20);
    CHECK(out.horizontal_accuracy == 2 && out.vertical_accuracy == 0 && out.speed_accuracy == 0);
    loc = location(100.75f); loc.horizontal_accuracy = 7;
    a.receive_location(42, loc, 30); out = prepare(a, 30); CHECK(out.horizontal_accuracy == 6);
    loc = location(101); loc.direction = 36100; loc.speed_horizontal = 25500;
    loc.altitude_geodetic = -1000; loc.latitude = loc.longitude = 0;
    a.receive_location(42, loc, 40); out = prepare(a, 40);
    CHECK(out.unknown_fields & CN46750_UNKNOWN_TRACK_ANGLE);
    CHECK(out.unknown_fields & CN46750_UNKNOWN_GROUND_SPEED);
    CHECK(out.unknown_fields & CN46750_UNKNOWN_UAV_POSITION);
    CHECK(out.unknown_fields & CN46750_UNKNOWN_UAV_ALTITUDE);
    // ODID fixed GCS location is the GB remote-station type, not takeoff type.
    CNData fixed; auto sys = system_message(); sys.operator_location_type = 2;
    fixed.select_source(42); fixed.receive_location(42, location(), 10); fixed.receive_system(42, sys, 10);
    out = prepare(fixed, 10); CHECK(out.unknown_fields == 0 && out.gcs_pos_type == POS_TYPE_GCS);
    // A known ODID coordinate -1e-7 cannot be emitted as GB's reserved sentinel.
    CNData reserved; loc = location(); loc.longitude = -1;
    reserved.select_source(42); reserved.receive_location(42, loc, 10); reserved.receive_system(42, system_message(), 10);
    out = prepare(reserved, 10); CHECK(out.unknown_fields & CN46750_UNKNOWN_UAV_POSITION);
}

static void quantization()
{
    // The adapter starts from integer centidegrees/centimetres per second.
    // Check its division and float conversion together with the actual codec.
    for (unsigned raw = 0; raw <= 35999; ++raw) {
        CNData a; a.select_source(42);
        auto loc = location(); loc.direction = uint16_t(raw);
        loc.speed_horizontal = uint16_t(raw <= 25425 ? raw : 25425);
        a.receive_location(42, loc, 10); a.receive_system(42, system_message(), 10);
        DroneRIDData_t out {}; CHECK(a.prepare(config(), 10, out));
        uint8_t frame[128]; size_t len;
        CHECK(CN46750_RID_Encode(&out, frame, sizeof(frame), &len) == RID_OK);
        CHECK((unsigned(frame[54]) | (unsigned(frame[55]) << 8)) == raw / 10);
        CHECK((unsigned(frame[56]) | (unsigned(frame[57]) << 8)) == (raw <= 25425 ? raw : 25425) / 10);
    }
}

int main()
{
    configuration(); source_isolation(); first_zero_and_duplicates();
    out_of_order_and_hour_wrap(); clocks_and_staleness(); quantization(); field_mapping();
    std::puts("CNData source isolation, time reconstruction, stale recovery and wire encoding passed");
}
