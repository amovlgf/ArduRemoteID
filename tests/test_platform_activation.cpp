#include "RemoteIDModule/platform_activation.h"
#include <algorithm>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

using namespace rid_activation;
using platform_activation::Manager;
using platform_activation::Source;
using Bytes = std::vector<uint8_t>;
static const uint8_t UID[] = {0xaa,0xbb,0xcc,0xdd,0xee,0xff};

#define CHECK(condition) do { if (!(condition)) throw std::runtime_error( \
    std::string(__func__) + ":" + std::to_string(__LINE__) + " " #condition); } while (false)

struct Memory : Storage {
    std::map<std::string, Bytes> data;
    std::vector<std::string> reads;
    std::string fail_write, corrupt_write, fail_read, commit_then_fail, read_after_write_error;
    unsigned writes = 0;
    unsigned erases = 0;
    Read read(const char *key, uint8_t *out, size_t capacity, size_t &size) override {
        reads.emplace_back(key);
        if (key == fail_read) return Read::ERROR;
        auto it = data.find(key);
        if (it == data.end()) return Read::MISSING;
        size = it->second.size();
        if (size > capacity) return Read::ERROR;
        std::memcpy(out, it->second.data(), size);
        return Read::OK;
    }
    bool write(const char *key, const uint8_t *p, size_t size) override {
        ++writes;
        if (key == fail_write) return false;
        data[key] = Bytes(p, p + size);
        if (key == corrupt_write) data[key][0] ^= 1;
        if (key == read_after_write_error) fail_read=key;
        return key != commit_then_fail;
    }
    bool erase(const char *key) override { ++erases; data.erase(key); return true; }
};

static Identity identity() {
    Identity id;
    id.revision = 7;
    std::memcpy(id.uid, UID, 6);
    id.aircraft_class = 2;
    std::strcpy(id.product, "AMOV0123456789012345");
    std::strcpy(id.registration, "REG-12345678");
    std::strcpy(id.uin, "12345678");
    identity_hash(id);
    return id;
}

static Bytes factory_record(Source source = Source::FACTORY) {
    Bytes bytes(platform_activation::FACTORY_SIZE);
    platform_activation::encode_factory(identity(), source, bytes.data());
    return bytes;
}

static void repair_crc(Bytes &bytes) {
    const size_t offset = bytes.size() - 4;
    const uint32_t crc = platform_activation::crc32(bytes.data(), offset);
    for (unsigned i = 0; i < 4; ++i) bytes[offset + i] = uint8_t(crc >> (8 * i));
}

static void factory_record_validation() {
    const Bytes valid = factory_record();
    Identity decoded;
    Source source;
    CHECK(platform_activation::decode_factory(valid.data(), valid.size(), UID, decoded, source));
    CHECK(source == Source::FACTORY && decoded.revision == 7);
    CHECK(!std::strcmp(decoded.hash, identity().hash));
    for (size_t length = 0; length < valid.size(); ++length)
        CHECK(!platform_activation::decode_factory(valid.data(), length, UID, decoded, source));
    CHECK(!platform_activation::decode_factory(nullptr, valid.size(), UID, decoded, source));
    Bytes oversized = valid; oversized.push_back(0);
    CHECK(!platform_activation::decode_factory(oversized.data(), oversized.size(), UID, decoded, source));
    for (size_t offset = 0; offset < valid.size(); ++offset) {
        Bytes bytes = valid; bytes[offset] ^= 1;
        CHECK(!platform_activation::decode_factory(bytes.data(), bytes.size(), UID, decoded, source));
    }
    for (unsigned invalid = 0; invalid < 13; ++invalid) {
        Bytes bytes = valid;
        switch (invalid) {
        case 0: bytes[8] = 2; break;
        case 1: bytes[9] = 5; break;
        case 2: bytes[10] = 65; break;
        case 3: bytes[11] = 7; break;
        case 4: std::fill(bytes.begin() + 12, bytes.begin() + 16, 0); break;
        case 5: bytes[16] ^= 1; break;
        case 6: bytes[22] = 0; break;
        case 7: bytes[54] = 'X'; break; // Nonzero registration padding.
        case 8: bytes[106] ^= 1; break; // Registration and suffix disagree.
        case 9: bytes[114] = 2; break; // Platform is a state source, not a factory source.
        case 10: bytes[115] = 1; break;
        case 11: bytes[42] = 31; break;
        case 12: bytes[11] = 0; std::fill(bytes.begin() + 106, bytes.begin() + 114, 0); break;
        }
        repair_crc(bytes);
        decoded = identity();
        CHECK(!platform_activation::decode_factory(bytes.data(), bytes.size(), UID, decoded, source));
        CHECK(decoded.revision == 0 && decoded.product[0] == 0);
    }
    uint8_t zero_uid[6] {};
    Bytes bytes = valid;
    std::fill(bytes.begin() + 16, bytes.begin() + 22, 0); repair_crc(bytes);
    CHECK(!platform_activation::decode_factory(bytes.data(), bytes.size(), zero_uid, decoded, source));
    Identity no_registration = identity(); no_registration.registration[0] = 0; no_registration.uin[0] = 0;
    platform_activation::encode_factory(no_registration, Source::FACTORY, bytes.data());
    CHECK(platform_activation::decode_factory(bytes.data(), bytes.size(), UID, decoded, source));
    CHECK(decoded.registration[0] == 0 && decoded.uin[0] == 0);
}

static void factory_identity_never_activates() {
    for (const auto source : {Source::FACTORY, Source::IMPORTED}) {
        Memory storage; storage.data["factory"] = factory_record(source);
        const Bytes original = storage.data["factory"];
        Manager fresh(UID, 11); fresh.boot(storage);
        CHECK(fresh.state().provisioned && fresh.state().snapshot.state == State::UNACTIVATED);
        CHECK(fresh.state().source == source && fresh.state().generation == 1);
        CHECK(fresh.state().transaction == 0 && fresh.state().snapshot.pending_revision == 0);
        CHECK(!std::strcmp(fresh.state().snapshot.identity.product, identity().product));
        CHECK(storage.data["factory"] == original && storage.erases == 0);
        const auto records = storage.data;
        const unsigned writes = storage.writes;
        Manager restarted(UID, 12); restarted.boot(storage);
        CHECK(restarted.state().snapshot.state == State::UNACTIVATED);
        CHECK(restarted.state().snapshot.boot_id == 12);
        CHECK(records == storage.data && writes == storage.writes);
    }
}

static void old_authorizations_are_ignored() {
    Memory storage;
    // These keys belong to the retired authorization format; boot must not
    // inspect, migrate, overwrite or delete them, even if no factory ID exists.
    for (const char *key : {"active_a", "active_b", "pending"}) storage.data[key] = Bytes(204, 0x5a);
    const auto records = storage.data;
    Manager blank(UID, 0); blank.boot(storage);
    CHECK(!blank.state().provisioned && blank.state().snapshot.state == State::UNACTIVATED);
    CHECK(blank.state().snapshot.boot_id != 0);
    const std::vector<std::string> expected_reads={"factory","state_a","state_b","ready"};
    CHECK(storage.reads == expected_reads);
    CHECK(blank.state().provision_available);
    CHECK(storage.data == records && storage.writes == 0 && storage.erases == 0);
    CHECK(blank.synchronize(storage, identity().product, true, 1) == Result::WRONG_DEVICE);
}

static void platform_sync_and_restart() {
    Memory storage; storage.data["factory"] = factory_record();
    const Identity id = identity();
    Manager fresh(UID, 11); fresh.boot(storage);
    const auto original_factory = storage.data["factory"];
    CHECK(fresh.synchronize(storage, id.product, true, 0) == Result::INVALID_FORMAT);
    CHECK(fresh.synchronize(storage, "WRONG012345678901234", true, 42) == Result::WRONG_DEVICE);
    CHECK(fresh.synchronize(storage, id.product, true, 42) == Result::OK);
    CHECK(fresh.state().snapshot.state == State::ACTIVE && fresh.state().source == Source::PLATFORM);
    const unsigned writes = storage.writes;
    CHECK(fresh.synchronize(storage, id.product, true, 42) == Result::OK && storage.writes == writes);
    CHECK(fresh.synchronize(storage, id.product, false, 42) == Result::VERSION_CONFLICT);
    Manager active(UID, 12); active.boot(storage);
    CHECK(active.state().snapshot.state == State::ACTIVE && active.state().transaction == 42);
    CHECK(active.synchronize(storage, id.product, false, 43) == Result::OK);
    Manager inactive(UID, 13); inactive.boot(storage);
    CHECK(inactive.state().snapshot.state == State::UNACTIVATED && inactive.state().transaction == 43);
    CHECK(!std::strcmp(inactive.state().snapshot.identity.hash, id.hash));
    CHECK(storage.data["factory"] == original_factory && storage.erases == 0);
    CHECK(inactive.synchronize(storage, id.product, true, 44) == Result::OK);
    inactive.boot(storage);
    CHECK(inactive.state().snapshot.state == State::ACTIVE && inactive.state().transaction == 44);
}

static void persisted_imported_state_is_preserved() {
    Memory storage; storage.data["factory"] = factory_record(Source::IMPORTED);
    Manager initial(UID, 1); initial.boot(storage);
    // Reproduce an already persisted record from the old migration, without
    // recreating its authorization parser or trusting the source flag alone.
    storage.data["state_a"][9] = uint8_t(State::ACTIVE);
    repair_crc(storage.data["state_a"]);
    const auto records = storage.data;
    const unsigned writes = storage.writes;
    Manager existing(UID, 2); existing.boot(storage);
    CHECK(existing.state().snapshot.state == State::ACTIVE && existing.state().source == Source::IMPORTED);
    CHECK(existing.state().generation == 1 && existing.state().transaction == 0);
    CHECK(records == storage.data && writes == storage.writes);
    CHECK(existing.synchronize(storage, identity().product, false, 100) == Result::OK);
    Manager deactivated(UID, 3); deactivated.boot(storage);
    CHECK(deactivated.state().snapshot.state == State::UNACTIVATED && deactivated.state().source == Source::PLATFORM);
    storage.data.erase("state_a"); storage.data.erase("state_b");
    Manager lost(UID, 4); lost.boot(storage);
    CHECK(lost.state().snapshot.state == State::INVALID && lost.state().snapshot.result == Result::STORAGE_ERROR);
}

static void rejected_factory_blocks_activation() {
    for (unsigned failure = 0; failure < 3; ++failure) {
        Memory storage; storage.data["factory"] = factory_record();
        if (failure == 0) storage.data["factory"][22] ^= 1;
        if (failure == 1) { storage.data["factory"][16] ^= 1; repair_crc(storage.data["factory"]); }
        if (failure == 2) storage.fail_read = "factory";
        const auto original = storage.data;
        Manager manager(UID, 21); manager.boot(storage);
        CHECK(!manager.state().provisioned && manager.state().snapshot.state == State::INVALID);
        CHECK(manager.state().snapshot.identity.revision == 0);
        CHECK(manager.synchronize(storage, identity().product, true, 1) == Result::WRONG_DEVICE);
        CHECK(storage.data == original && storage.writes == 0 && storage.erases == 0);
    }
}

static void storage_failures_and_recovery() {
    for (unsigned failure = 0; failure < 3; ++failure) {
        for (const char *target : {"state_a", "state_b"}) {
            Memory storage; storage.data["factory"] = factory_record();
            Manager manager(UID, 30); manager.boot(storage);
            if (!std::strcmp(target, "state_a")) CHECK(manager.synchronize(storage, identity().product, true, 8) == Result::OK);
            const auto before = manager.state();
            if (failure == 0) storage.fail_write = target;
            if (failure == 1) storage.corrupt_write = target;
            if (failure == 2) storage.fail_read = target;
            const bool activate = before.snapshot.state != State::ACTIVE;
            CHECK(manager.synchronize(storage, identity().product, activate, 9) == Result::STORAGE_ERROR);
            CHECK(manager.state().generation == before.generation && manager.state().snapshot.state == before.snapshot.state);
            storage.fail_write.clear(); storage.corrupt_write.clear(); storage.fail_read.clear();
            Manager recovered(UID, 31); recovered.boot(storage);
            CHECK(recovered.state().snapshot.state != State::INVALID);
            CHECK(recovered.state().transaction == (failure == 2 ? 9 : before.transaction));
            CHECK(!std::strcmp(recovered.state().snapshot.identity.hash, identity().hash));
        }
    }
    for (unsigned failure = 0; failure < 3; ++failure) {
        Memory storage; storage.data["factory"] = factory_record(Source::IMPORTED);
        if (failure == 0) storage.fail_write = "state_a";
        if (failure == 1) storage.corrupt_write = "state_a";
        if (failure == 2) storage.fail_write = "ready";
        Manager manager(UID, 40); manager.boot(storage);
        CHECK(manager.state().snapshot.state != State::ACTIVE);
        CHECK(manager.state().snapshot.result == Result::STORAGE_ERROR);
        CHECK(storage.data.count("factory") == 1 && storage.erases == 0);
    }
}

static void corrupt_state_and_conflicts() {
    Memory baseline; baseline.data["factory"] = factory_record();
    Manager initial(UID, 1); initial.boot(baseline);
    CHECK(initial.synchronize(baseline, identity().product, true, 42) == Result::OK);
    for (unsigned failure = 0; failure < 6; ++failure) {
        Memory storage = baseline;
        if (failure == 0) { storage.data["state_a"][0] ^= 1; storage.data["state_b"][0] ^= 1; }
        if (failure == 1) { storage.data.erase("state_a"); storage.data.erase("state_b"); }
        if (failure == 2) { storage.data["state_b"][12] = 1; repair_crc(storage.data["state_b"]); }
        if (failure == 3) { storage.data["state_a"][32] ^= 1; repair_crc(storage.data["state_a"]); storage.data.erase("state_b"); }
        if (failure == 4) { storage.data["state_b"][24] ^= 1; repair_crc(storage.data["state_b"]); storage.data.erase("state_a"); }
        if (failure == 5) { storage.data.erase("state_a"); storage.data.erase("state_b"); storage.data["ready"] = Bytes(4, 0); }
        Manager manager(UID, 2); manager.boot(storage);
        CHECK(manager.state().snapshot.state == State::INVALID);
        CHECK(manager.state().snapshot.result == (failure == 2 ? Result::VERSION_CONFLICT : Result::STORAGE_ERROR));
        const auto original=storage.data;
        CHECK(manager.synchronize(storage,identity().product,true,99)!=Result::OK);
        CHECK(storage.data==original && manager.state().snapshot.state==State::INVALID);
    }
    Memory overflow = baseline;
    std::fill(overflow.data["state_b"].begin() + 12, overflow.data["state_b"].begin() + 16, 0xff);
    repair_crc(overflow.data["state_b"]);
    Manager exhausted(UID, 3); exhausted.boot(overflow);
    CHECK(exhausted.synchronize(overflow, identity().product, false, 43) == Result::VERSION_CONFLICT);
    CHECK(exhausted.state().snapshot.state == State::ACTIVE);
}


static void first_provision_and_restart() {
    for (uint8_t aircraft_class=0; aircraft_class<=4; ++aircraft_class) {
        Memory storage;
        storage.data["PUBLIC_KEY4"]={1,2,3,4};
        storage.data["WIFI_SSID"]={5,6,7};
        Manager fresh(UID, 51); fresh.boot(storage);
        CHECK(fresh.state().provision_available);
        CHECK(fresh.provision(storage, identity().product, aircraft_class)==Result::OK);
        const auto state=fresh.state();
        CHECK(state.provisioned && !state.provision_available);
        CHECK(state.snapshot.state==State::UNACTIVATED && state.source==Source::FACTORY);
        CHECK(state.snapshot.identity.revision==1 && state.generation==1 && state.transaction==0);
        CHECK(state.snapshot.identity.aircraft_class==aircraft_class);
        CHECK(!std::memcmp(state.snapshot.identity.uid,UID,6));
        CHECK(!std::strcmp(state.snapshot.identity.product,identity().product));
        CHECK(!state.snapshot.identity.registration[0] && !state.snapshot.identity.uin[0]);
        CHECK(std::strlen(state.snapshot.identity.hash)==64);
        CHECK(storage.data.at("PUBLIC_KEY4")==Bytes({1,2,3,4}));
        CHECK(storage.data.at("WIFI_SSID")==Bytes({5,6,7}));
        CHECK(storage.erases==0);
        const auto original=storage.data;
        const unsigned writes=storage.writes;
        CHECK(fresh.provision(storage,identity().product,aircraft_class)==Result::VERSION_CONFLICT);
        CHECK(fresh.provision(storage,"DIFF0123456789012345",aircraft_class)==Result::VERSION_CONFLICT);
        CHECK(original==storage.data && writes==storage.writes);
        Manager rebooted(UID,52); rebooted.boot(storage);
        CHECK(rebooted.state().provisioned && !rebooted.state().provision_available);
        CHECK(rebooted.state().snapshot.state==State::UNACTIVATED);
        CHECK(!std::strcmp(rebooted.state().snapshot.identity.hash,state.snapshot.identity.hash));
        CHECK(rebooted.provision(storage,identity().product,aircraft_class)==Result::VERSION_CONFLICT);
        CHECK(rebooted.synchronize(storage,identity().product,true,77)==Result::OK);
        CHECK(rebooted.provision(storage,identity().product,aircraft_class)==Result::VERSION_CONFLICT);
        CHECK(rebooted.state().snapshot.state==State::ACTIVE);
    }
}

static void provision_rejects_invalid_fields() {
    Memory storage;
    Manager manager(UID,61); manager.boot(storage);
    CHECK(manager.provision(storage,nullptr,2)==Result::INVALID_IDENTITY);
    CHECK(manager.provision(storage,identity().product,5)==Result::INVALID_IDENTITY);
    for (unsigned position=0; position<20; ++position) {
        for (const uint8_t invalid : {0,32,127,255}) {
            char sn[20]; std::memcpy(sn,identity().product,20); sn[position]=char(invalid);
            CHECK(manager.provision(storage,sn,2)==Result::INVALID_IDENTITY);
        }
    }
    CHECK(storage.writes==0 && storage.erases==0 && storage.data.empty());
    uint8_t zero_uid[6] {};
    Manager invalid(zero_uid,62); invalid.boot(storage);
    CHECK(invalid.provision(storage,identity().product,2)==Result::INVALID_IDENTITY);
    CHECK(storage.writes==0);
}

static void provision_requires_all_records_absent() {
    for (const char *key : {"factory","state_a","state_b","ready"}) {
        for (bool unreadable : {false,true}) {
            Memory storage;
            if (unreadable) storage.fail_read=key;
            else storage.data[key]=Bytes(1,0x11); // Even corrupt/empty historical records block.
            Manager manager(UID,71); manager.boot(storage);
            CHECK(!manager.state().provision_available && manager.state().snapshot.state==State::INVALID);
            const auto before=storage.data;
            CHECK(manager.provision(storage,identity().product,2)!=Result::OK);
            CHECK(storage.writes==0 && storage.erases==0 && storage.data==before);
        }
        // Recheck at execution after a previously clean boot/INFO response.
        Memory changed; Manager manager(UID,72); manager.boot(changed);
        CHECK(manager.state().provision_available);
        changed.data[key]=Bytes(1,0x11);
        const auto before=changed.data;
        CHECK(manager.provision(changed,identity().product,2)!=Result::OK);
        CHECK(!manager.state().provision_available);
        CHECK(changed.writes==0 && changed.data==before);
    }
}

static void provision_storage_failures_are_reconciled() {
    for (const char *key : {"factory","state_a","ready"}) {
        for (unsigned fault=0; fault<4; ++fault) {
            Memory storage; Manager manager(UID,81); manager.boot(storage);
            if (fault==0) storage.fail_write=key;
            if (fault==1) storage.corrupt_write=key;
            if (fault==2) storage.commit_then_fail=key;
            if (fault==3) storage.read_after_write_error=key;
            CHECK(manager.provision(storage,identity().product,2)==Result::STORAGE_ERROR);
            CHECK(manager.state().snapshot.state!=State::ACTIVE);
            const bool has_factory=storage.data.count("factory");
            CHECK(manager.state().provision_available==!has_factory);
            const auto factory=has_factory?storage.data.at("factory"):Bytes{};
            storage.fail_write.clear(); storage.corrupt_write.clear(); storage.commit_then_fail.clear();
            storage.read_after_write_error.clear(); storage.fail_read.clear();
            if (has_factory) {
                CHECK(manager.provision(storage,"DIFF0123456789012345",3)!=Result::OK);
                CHECK(storage.data.at("factory")==factory);
                CHECK(!manager.state().provision_available);
            } else {
                CHECK(manager.provision(storage,identity().product,2)==Result::OK);
            }
            Manager restarted(UID,82); restarted.boot(storage);
            CHECK(restarted.state().snapshot.state!=State::ACTIVE);
            CHECK(!restarted.state().provision_available && storage.erases==0);
            // A commit that reports failure after durable completion must expose
            // the committed factory identity immediately, not an empty cache.
            if (fault==2) {
                CHECK(restarted.state().provisioned);
                CHECK(!std::strcmp(restarted.state().snapshot.identity.product,identity().product));
            }
        }
    }
}

int main(int argc, char **argv) {
    const std::map<std::string, void (*)()> cases = {
        {"factory_validation", factory_record_validation},
        {"first_provision", first_provision_and_restart},
        {"provision_invalid", provision_rejects_invalid_fields},
        {"provision_history", provision_requires_all_records_absent},
        {"provision_failures", provision_storage_failures_are_reconciled},
        {"factory_initialization", factory_identity_never_activates},
        {"retired_authorizations", old_authorizations_are_ignored},
        {"platform_sync", platform_sync_and_restart},
        {"persisted_imported", persisted_imported_state_is_preserved},
        {"invalid_factory", rejected_factory_blocks_activation},
        {"storage_recovery", storage_failures_and_recovery},
        {"state_conflicts", corrupt_state_and_conflicts},
    };
    try {
        if (argc == 2) {
            std::ifstream input(argv[1], std::ios::binary);
            Bytes bytes((std::istreambuf_iterator<char>(input)), {});
            Identity id; Source source;
            CHECK(platform_activation::decode_factory(bytes.data(), bytes.size(), UID, id, source));
            CHECK(source == Source::FACTORY);
            std::cout << id.hash << '\n';
            return 0;
        }
        if (argc == 3 && std::string(argv[1]) == "--case") { cases.at(argv[2])(); return 0; }
        for (const auto &test : cases) test.second();
        std::cout << "Factory identity, platform synchronization and durable state recovery passed\n";
    } catch (const std::exception &error) { std::cerr << error.what() << '\n'; return 1; }
    return 0;
}
