#include "RemoteIDModule/activation.h"
#include "RemoteIDModule/monocypher.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

using namespace rid_activation;
using Package = std::array<uint8_t, PACKAGE_SIZE>;
static const uint8_t UID[6] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};
static uint8_t test_secret[32];
static TrustKey test_key;

#define CHECK(condition) do { if (!(condition)) throw std::runtime_error( \
    std::string(__func__) + ":" + std::to_string(__LINE__) + " " #condition); } while (false)

static void resign(Package &p)
{
    crypto_sign(p.data() + SIGNED_SIZE, test_secret, test_key.public_key, p.data(), SIGNED_SIZE);
}

static Package package(uint32_t revision = 1)
{
    Package p{};
    std::memcpy(p.data(), "AMOVACT1", 8);
    p[8] = 1; p[9] = 1; p[10] = 255; p[11] = 255; p[12] = 13; p[14] = 1;
    for (unsigned i = 0; i < 4; ++i) p[16 + i] = uint8_t(revision >> (8 * i));
    std::memcpy(p.data() + 20, UID, 6);
    p[26] = 2; p[27] = 12; p[28] = 8;
    std::fill(p.begin() + 32, p.begin() + 48, 1);
    std::memcpy(p.data() + 48, "AMOV0123456789012345", 20);
    std::memcpy(p.data() + 68, "REG-12345678", 12);
    std::memcpy(p.data() + 132, "12345678", 8);
    resign(p);
    return p;
}

static Result validate(const Package &p)
{
    Identity identity;
    return verify(p.data(), p.size(), &test_key, 1, UID, 13, identity);
}

class MemoryStorage : public Storage {
public:
    std::map<std::string, std::vector<uint8_t>> records;
    std::string fail_write;
    std::string fail_read;
    std::string fail_erase;
    std::string corrupt_write;
    unsigned writes = 0;

    Read read(const char *key, uint8_t *bytes, size_t capacity, size_t &size) override
    {
        if (fail_read == key) return Read::ERROR;
        const auto it = records.find(key);
        if (it == records.end()) return Read::MISSING;
        size = it->second.size();
        if (size > capacity) return Read::ERROR;
        std::copy(it->second.begin(), it->second.end(), bytes);
        return Read::OK;
    }
    bool write(const char *key, const uint8_t *bytes, size_t size) override
    {
        ++writes;
        if (fail_write == key) return false;
        records[key] = std::vector<uint8_t>(bytes, bytes + size);
        if (corrupt_write == key) records[key].resize(size / 2);
        return true;
    }
    bool erase(const char *key) override
    {
        if (fail_erase == key) return false;
        records.erase(key);
        return true;
    }
    void put(const char *key, const Package &p) { records[key] = {p.begin(), p.end()}; }
};

static void signature_and_identity()
{
    const Package p = package();
    Identity identity;
    CHECK(verify(p.data(), p.size(), &test_key, 1, UID, 13, identity) == Result::OK);
    CHECK(identity.revision == 1 && identity.aircraft_class == 2);
    CHECK(std::string(identity.product) == "AMOV0123456789012345");
    CHECK(std::string(identity.registration) == "REG-12345678");
    CHECK(std::string(identity.uin) == "12345678");
    CHECK(std::string(identity.hash) == "A87E0E95C13C94790A4B152D027681313B5D133A1B19541AFCEE0D2C0955A300");
    CHECK(verify(p.data(), p.size(), nullptr, 0, UID, 13, identity) == Result::NO_TRUST);
    CHECK(identity.revision == 0 && identity.product[0] == 0);
    CHECK(verify(p.data(), p.size(), &test_key, 1, UID, 14, identity) == Result::WRONG_DEVICE);
    uint8_t other[6]; std::memcpy(other, UID, 6); ++other[0];
    CHECK(verify(p.data(), p.size(), &test_key, 1, other, 13, identity) == Result::WRONG_DEVICE);
    CHECK(verify(nullptr, 0, &test_key, 1, UID, 13, identity) == Result::INVALID_FORMAT);
    for (size_t length = 0; length < p.size(); ++length)
        CHECK(verify(p.data(), length, &test_key, 1, UID, 13, identity) == Result::INVALID_FORMAT);
    std::vector<uint8_t> too_long(p.begin(), p.end()); too_long.push_back(0);
    CHECK(verify(too_long.data(), too_long.size(), &test_key, 1, UID, 13, identity) == Result::INVALID_FORMAT);
    for (size_t offset = 0; offset < p.size(); ++offset) {
        Package changed = p; changed[offset] ^= 1;
        CHECK(validate(changed) != Result::OK);
    }
}

static void signed_invalid_payloads()
{
    for (unsigned offset : {26U, 27U, 28U, 48U, 80U, 139U}) {
        Package p = package();
        if (offset == 26) p[offset] = 5;
        else if (offset == 27) p[offset] = 65;
        else if (offset == 28) p[offset] = 7;
        else if (offset == 48) p[offset] = 0;
        else p[offset] ^= 1;
        resign(p);
        CHECK(validate(p) == Result::INVALID_IDENTITY);
    }
    CHECK(validate(package(0)) == Result::INVALID_IDENTITY);
    Package no_token = package(); std::fill(no_token.begin() + 32, no_token.begin() + 48, 0); resign(no_token);
    CHECK(validate(no_token) == Result::INVALID_IDENTITY);
    Package unknown = package(); unknown[27] = 0; unknown[28] = 0;
    std::fill(unknown.begin() + 68, unknown.begin() + 140, 0); resign(unknown);
    CHECK(validate(unknown) == Result::OK);
    Package full = package(); full[27] = 64;
    std::fill(full.begin() + 68, full.begin() + 132, 'A');
    std::memcpy(full.data() + 124, "12345678", 8); resign(full);
    CHECK(validate(full) == Result::OK);
}

static void cold_start_and_idempotency()
{
    MemoryStorage storage;
    Manager first(&test_key, 1, UID, 13, 100);
    first.boot(storage, true);
    CHECK(first.snapshot().state == State::UNACTIVATED);
    const Package p = package();
    CHECK(first.stage(storage, p.data(), p.size()) == Result::PENDING_REBOOT);
    CHECK(first.snapshot().state == State::UNACTIVATED && first.snapshot().identity.revision == 0);
    CHECK(first.snapshot().pending_revision == 1);
    const unsigned writes = storage.writes;
    CHECK(first.stage(storage, p.data(), p.size()) == Result::PENDING_REBOOT);
    CHECK(storage.writes == writes);
    Manager warm(&test_key, 1, UID, 13, 101); warm.boot(storage, false);
    CHECK(warm.snapshot().state == State::UNACTIVATED && warm.snapshot().pending_revision == 1);
    Manager cold(&test_key, 1, UID, 13, 102); cold.boot(storage, true);
    CHECK(cold.snapshot().state == State::ACTIVE && cold.snapshot().identity.revision == 1);
    CHECK(cold.snapshot().pending_revision == 0 && storage.records.count("pending") == 0);
    const unsigned active_writes = storage.writes;
    CHECK(cold.stage(storage, p.data(), p.size()) == Result::ALREADY_ACTIVE);
    CHECK(storage.writes == active_writes);
    Manager restarted(&test_key, 1, UID, 13, 103); restarted.boot(storage, false);
    CHECK(restarted.snapshot().state == State::ACTIVE);
    CHECK(restarted.snapshot().boot_id == 103);
    CHECK(std::string(restarted.snapshot().identity.hash) == cold.snapshot().identity.hash);
}

static void update_and_revision_rules()
{
    MemoryStorage storage; storage.put("active_a", package());
    Manager manager(&test_key, 1, UID, 13, 100); manager.boot(storage, false);
    Package next = package(2); next[48] = 'Z'; resign(next);
    CHECK(manager.stage(storage, next.data(), next.size()) == Result::PENDING_REBOOT);
    CHECK(manager.snapshot().identity.revision == 1 && manager.snapshot().identity.product[0] == 'A');
    Package conflict = next; conflict[49] = 'Y'; resign(conflict);
    CHECK(manager.stage(storage, conflict.data(), conflict.size()) == Result::VERSION_CONFLICT);
    Package old = package();
    CHECK(manager.stage(storage, old.data(), old.size()) == Result::OLD_REVISION);
    Manager warm(&test_key, 1, UID, 13, 101); warm.boot(storage, false);
    CHECK(warm.snapshot().identity.revision == 1 && warm.snapshot().pending_revision == 2);
    Manager cold(&test_key, 1, UID, 13, 102); cold.boot(storage, true);
    CHECK(cold.snapshot().identity.revision == 2 && cold.snapshot().identity.product[0] == 'Z');
    CHECK(cold.stage(storage, old.data(), old.size()) == Result::OLD_REVISION);
    CHECK(cold.stage(storage, conflict.data(), conflict.size()) == Result::VERSION_CONFLICT);
}

static void storage_failure_atomicity()
{
    Package next = package(2); next[48] = 'Z'; resign(next);
    for (int failure = 0; failure < 4; ++failure) {
        MemoryStorage storage; storage.put("active_a", package()); storage.put("pending", next);
        if (failure == 0) storage.fail_write = "active_b";
        if (failure == 1) storage.corrupt_write = "active_b";
        if (failure == 2) storage.fail_read = "active_b";
        if (failure == 3) storage.fail_erase = "pending";
        Manager current(&test_key, 1, UID, 13, 100); current.boot(storage, true);
        CHECK(current.snapshot().result == Result::STORAGE_ERROR);
        CHECK(current.snapshot().state == State::ACTIVE);
        if (failure < 3) CHECK(current.snapshot().identity.revision == 1);
        storage.fail_write.clear(); storage.corrupt_write.clear(); storage.fail_read.clear(); storage.fail_erase.clear();
        Manager restored(&test_key, 1, UID, 13, 101); restored.boot(storage, false);
        CHECK(restored.snapshot().state == State::ACTIVE);
        CHECK(restored.snapshot().identity.revision == 1 || restored.snapshot().identity.revision == 2);
        CHECK(restored.snapshot().identity.product[0] == (restored.snapshot().identity.revision == 1 ? 'A' : 'Z'));
        Identity expected_identity;
        const Package expected = restored.snapshot().identity.revision == 1 ? package() : next;
        CHECK(verify(expected.data(), expected.size(), &test_key, 1, UID, 13, expected_identity) == Result::OK);
        CHECK(std::string(restored.snapshot().identity.hash) == expected_identity.hash);
    }
    MemoryStorage storage; storage.put("active_a", package()); storage.fail_write = "pending";
    Manager active(&test_key, 1, UID, 13, 300); active.boot(storage, false);
    CHECK(active.stage(storage, next.data(), next.size()) == Result::STORAGE_ERROR);
    CHECK(active.snapshot().identity.revision == 1 && active.snapshot().pending_revision == 0);
    CHECK(storage.records.count("pending") == 0);
}

static void damaged_and_conflicting_records()
{
    MemoryStorage storage; storage.put("active_a", package());
    storage.records["active_a"][70] ^= 1;
    Manager damaged(&test_key, 1, UID, 13, 50); damaged.boot(storage, false);
    CHECK(damaged.snapshot().state == State::INVALID && damaged.snapshot().identity.revision == 0);
    storage.put("active_b", package(2));
    Manager recovered(&test_key, 1, UID, 13, 51); recovered.boot(storage, false);
    CHECK(recovered.snapshot().state == State::ACTIVE && recovered.snapshot().identity.revision == 2);
    Package conflict = package(2); conflict[48] = 'X'; resign(conflict);
    storage.put("active_a", conflict);
    Manager ambiguous(&test_key, 1, UID, 13, 0x12345678); ambiguous.boot(storage, false);
    CHECK(ambiguous.snapshot().state == State::INVALID);
    CHECK(ambiguous.snapshot().result == Result::VERSION_CONFLICT);
    CHECK(ambiguous.snapshot().boot_id == 0x12345678);
    CHECK(std::memcmp(ambiguous.snapshot().uid, UID, 6) == 0);
}

static void read_only_snapshot()
{
    MemoryStorage storage; storage.put("active_a", package());
    Manager active(&test_key, 1, UID, 13, 0x12345678); active.boot(storage, false);
    ParamValue value;
    const Snapshot &snapshot = active.snapshot();
    CHECK(get_parameter(snapshot, "RID_ID_SCHEMA", value) && !value.is_string && value.integer == 1);
    CHECK(get_parameter(snapshot, "RID_BOOT_ID", value) && value.integer == 0x12345678);
    CHECK(get_parameter(snapshot, "RID_UID", value) && value.is_string && std::string(value.string) == "AABBCCDDEEFF");
    CHECK(get_parameter(snapshot, "RID_ID_HASH", value) && value.is_string && std::strlen(value.string) == 64);
    CHECK(get_parameter(snapshot, "RID_UAS_ID", value) && std::strlen(value.string) == 20);
    CHECK(get_parameter(snapshot, "RID_REG_ID", value) && std::string(value.string) == "REG-12345678");
    CHECK(get_parameter(snapshot, "RID_UIN", value) && std::string(value.string) == "12345678");
    CHECK(get_parameter(snapshot, "RID_CLASS", value) && value.integer == 2);
    CHECK(!get_parameter(snapshot, "UNRELATED", value));
    for (const char *name : {"RID_ID_SCHEMA", "RID_ID_REV", "RID_BOOT_ID", "RID_ACT_STATE", "RID_UAS_ID", "RID_REG_ID",
                            "RID_UIN", "RID_CLASS", "RID_UID", "RID_ID_HASH", "RID_PENDING", "RID_ACT_ERR",
                            "CN_UAS_ID", "CN_UIN", "CN_CLASS"}) CHECK(protected_parameter(name));
    CHECK(!protected_parameter("CN_CAN_NODE"));
    CHECK(!protected_parameter("WIFI_CHAN"));
}

int main(int argc, char **argv)
{
    for (unsigned i = 0; i < 32; ++i) test_secret[i] = uint8_t(i);
    test_key.id = TEST_KEY_ID;
    crypto_sign_public_key(test_key.public_key, test_secret);
    if (argc == 2) {
        std::ifstream stream(argv[1], std::ios::binary);
        std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
        Identity identity;
        if (verify(bytes.data(), bytes.size(), &test_key, 1, UID, 13, identity) != Result::OK) return 2;
        std::cout << identity.hash << '\n';
        return 0;
    }
    unsigned failed = 0;
    for (auto test : {signature_and_identity, signed_invalid_payloads, cold_start_and_idempotency,
                     update_and_revision_rules, storage_failure_atomicity, damaged_and_conflicting_records, read_only_snapshot}) {
        try { test(); }
        catch (const std::exception &error) { std::cerr << error.what() << '\n'; ++failed; }
    }
    if (failed) return 1;
    std::cout << "Activation: signature, identity, restart, storage fault and read-only snapshot tests passed\n";
    return 0;
}
