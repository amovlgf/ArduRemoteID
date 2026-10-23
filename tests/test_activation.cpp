#include "RemoteIDModule/activation.h"

#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>

using namespace rid_activation;
static const uint8_t UID[6] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};

#define CHECK(condition) do { if (!(condition)) throw std::runtime_error( \
    std::string(__func__) + ":" + std::to_string(__LINE__) + " " #condition); } while (false)

static Identity identity()
{
    Identity id;
    id.revision = 1;
    std::memcpy(id.uid, UID, sizeof(UID));
    id.aircraft_class = 2;
    std::strcpy(id.product, "AMOV0123456789012345");
    std::strcpy(id.registration, "REG-12345678");
    std::strcpy(id.uin, "12345678");
    identity_hash(id);
    return id;
}

static void stable_identity_hash()
{
    const Identity original = identity();
    CHECK(std::string(original.hash) == "A87E0E95C13C94790A4B152D027681313B5D133A1B19541AFCEE0D2C0955A300");
    for (unsigned field = 0; field < 6; ++field) {
        Identity changed = original;
        if (field == 0) ++changed.revision;
        if (field == 1) ++changed.uid[0];
        if (field == 2) ++changed.aircraft_class;
        if (field == 3) changed.product[0] = 'X';
        if (field == 4) changed.registration[0] = 'X';
        if (field == 5) changed.uin[0] = 'X';
        identity_hash(changed);
        CHECK(std::strcmp(original.hash, changed.hash));
    }
}

static void read_only_snapshot()
{
    Snapshot snapshot;
    snapshot.identity = identity();
    std::memcpy(snapshot.uid, UID, sizeof(UID));
    snapshot.boot_id = 0x12345678;
    snapshot.state = State::ACTIVE;
    ParamValue value;
    CHECK(get_parameter(snapshot, "RID_ID_SCHEMA", value) && !value.is_string && value.integer == 1);
    CHECK(get_parameter(snapshot, "RID_ID_REV", value) && value.integer == 1);
    CHECK(get_parameter(snapshot, "RID_BOOT_ID", value) && value.integer == 0x12345678);
    CHECK(get_parameter(snapshot, "RID_UID", value) && value.is_string && std::string(value.string) == "AABBCCDDEEFF");
    CHECK(get_parameter(snapshot, "RID_ID_HASH", value) && value.is_string && std::string(value.string) == snapshot.identity.hash);
    CHECK(get_parameter(snapshot, "RID_UAS_ID", value) && std::string(value.string) == "AMOV0123456789012345");
    CHECK(get_parameter(snapshot, "RID_REG_ID", value) && std::string(value.string) == "REG-12345678");
    CHECK(get_parameter(snapshot, "RID_UIN", value) && std::string(value.string) == "12345678");
    CHECK(get_parameter(snapshot, "RID_CLASS", value) && !value.is_string && value.integer == 2);
    CHECK(get_parameter(snapshot, "RID_ACT_STATE", value) && value.integer == 1);
    CHECK(get_parameter(snapshot, "RID_PENDING", value) && value.integer == 0);
    snapshot.result = Result::STORAGE_ERROR;
    CHECK(get_parameter(snapshot, "RID_ACT_ERR", value) && value.integer == 10);
    CHECK(!get_parameter(snapshot, "UNRELATED", value));
    for (const char *name : {"RID_ID_SCHEMA", "RID_ID_REV", "RID_BOOT_ID", "RID_ACT_STATE", "RID_UAS_ID", "RID_REG_ID",
                            "RID_UIN", "RID_CLASS", "RID_UID", "RID_ID_HASH", "RID_PENDING", "RID_ACT_ERR",
                            "CN_UAS_ID", "CN_UIN", "CN_CLASS"}) CHECK(protected_parameter(name));
    CHECK(!protected_parameter("CN_CAN_NODE"));
    CHECK(!protected_parameter("WIFI_CHAN"));
    CHECK(get_parameter(Snapshot{}, "RID_UAS_ID", value) && value.string[0] == 0);
    CHECK(get_parameter(Snapshot{}, "RID_ACT_STATE", value) && value.integer == 0);
}

static void wire_result_compatibility()
{
    const Result values[] = {Result::OK, Result::PENDING_REBOOT, Result::ALREADY_ACTIVE,
        Result::INVALID_FORMAT, Result::NO_TRUST, Result::INVALID_SIGNATURE, Result::WRONG_DEVICE,
        Result::INVALID_IDENTITY, Result::OLD_REVISION, Result::VERSION_CONFLICT, Result::STORAGE_ERROR, Result::BUSY};
    const char *names[] = {"OK", "PENDING_REBOOT", "ALREADY_ACTIVE", "INVALID_FORMAT", "NO_TRUST",
        "INVALID_SIGNATURE", "WRONG_DEVICE", "INVALID_IDENTITY", "OLD_REVISION", "VERSION_CONFLICT", "STORAGE_ERROR", "BUSY"};
    for (unsigned i = 0; i < sizeof(values) / sizeof(values[0]); ++i) {
        CHECK(uint8_t(values[i]) == i);
        CHECK(std::strcmp(result_name(values[i]), names[i]) == 0);
    }
    CHECK(std::strcmp(result_name(Result(255)), "UNKNOWN") == 0);
}

int main(int argc, char **argv)
{
    if (argc == 4) {
        Identity id = identity();
        id.revision = uint32_t(std::stoul(argv[1]));
        if (std::strlen(argv[2]) > 64 || std::strlen(argv[3]) > 8) return 2;
        std::memset(id.registration, 0, sizeof(id.registration));
        std::memset(id.uin, 0, sizeof(id.uin));
        std::strcpy(id.registration, argv[2]);
        std::strcpy(id.uin, argv[3]);
        identity_hash(id);
        std::cout << id.hash << '\n';
        return 0;
    }
    unsigned failed = 0;
    for (auto test : {stable_identity_hash, read_only_snapshot, wire_result_compatibility}) {
        try { test(); }
        catch (const std::exception &error) { std::cerr << error.what() << '\n'; ++failed; }
    }
    if (failed) return 1;
    std::cout << "Identity hash, read-only snapshot and management result compatibility passed\n";
    return 0;
}
