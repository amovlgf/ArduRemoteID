#include "activation.h"
#include "monocypher.h"
#include <string.h>

namespace rid_activation {
namespace {
const uint8_t MAGIC[8] = {'A','M','O','V','A','C','T','1'};
uint16_t le16(const uint8_t *p) { return uint16_t(p[0]) | uint16_t(p[1]) << 8; }
uint32_t le32(const uint8_t *p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }
bool printable(const uint8_t *p, size_t len) {
    for (size_t i=0; i<len; ++i) if (p[i] < 33 || p[i] > 126) return false;
    return true;
}
bool zero(const uint8_t *p, size_t len) {
    for (size_t i=0; i<len; ++i) if (p[i] != 0) return false;
    return true;
}
const char *const slots[] = {"active_a", "active_b"};
}

void hex_encode(const uint8_t *bytes, size_t count, char *out) {
    const char *hex = "0123456789ABCDEF";
    for (size_t i=0; i<count; ++i) { out[2*i] = hex[bytes[i] >> 4]; out[2*i+1] = hex[bytes[i] & 15]; }
    out[2*count] = 0;
}

void identity_hash(Identity &id) {
    uint8_t data[115] {};
    memcpy(data, "RID-ID-V1", 9);
    for (unsigned i=0; i<4; ++i) data[10+i] = uint8_t(id.revision >> (8*i));
    memcpy(data+14, id.uid, 6);
    data[20] = id.aircraft_class;
    memcpy(data+21, id.product, 20);
    data[41] = uint8_t(strlen(id.registration));
    memcpy(data+42, id.registration, data[41]);
    data[106] = uint8_t(strlen(id.uin));
    memcpy(data+107, id.uin, data[106]);
    uint8_t digest[32];
    crypto_blake2b_general(digest, sizeof(digest), nullptr, 0, data, sizeof(data));
    hex_encode(digest, sizeof(digest), id.hash);
}

Result verify(const uint8_t *p, size_t size, const TrustKey *keys, size_t key_count,
              const uint8_t uid[6], uint16_t board, Identity &identity) {
    identity = Identity{};
    if (!p || size != PACKAGE_SIZE || memcmp(p, MAGIC, 8) || p[8] != 1 || p[9] != 1 ||
        le16(p+14) != 1 || !zero(p+29, 3)) return Result::INVALID_FORMAT;
    const TrustKey *key = nullptr;
    for (size_t i=0; i<key_count; ++i) if (keys[i].id == le16(p+10)) { key = &keys[i]; break; }
    if (!key) return Result::NO_TRUST;
    // Deliberately independent of OTA/Transport's no-key and LOCK_LEVEL exceptions.
    if (crypto_check(p+SIGNED_SIZE, key->public_key, p, SIGNED_SIZE) != 0) return Result::INVALID_SIGNATURE;
    if (le16(p+12) != board || memcmp(p+20, uid, 6) || zero(uid, 6)) return Result::WRONG_DEVICE;
    const size_t reg_len=p[27], uin_len=p[28];
    if (le32(p+16) == 0 || p[26] > 4 || reg_len > 64 || (uin_len != 0 && uin_len != 8) ||
        zero(p+32, 16) || !printable(p+48, 20) || !printable(p+68, reg_len) ||
        !zero(p+68+reg_len, 64-reg_len) || !printable(p+132, uin_len) || !zero(p+132+uin_len, 8-uin_len))
        return Result::INVALID_IDENTITY;
    // A known complete registration must agree with its broadcast suffix. An
    // independently supplied suffix never fabricates a complete registration.
    if (reg_len && (reg_len < 8 || uin_len != 8 || memcmp(p+68+reg_len-8, p+132, 8)))
        return Result::INVALID_IDENTITY;
    identity.revision = le32(p+16);
    memcpy(identity.uid, uid, 6);
    identity.aircraft_class = p[26];
    memcpy(identity.product, p+48, 20);
    memcpy(identity.registration, p+68, reg_len);
    memcpy(identity.uin, p+132, uin_len);
    identity_hash(identity);
    return Result::OK;
}

Manager::Manager(const TrustKey *keys, size_t count, const uint8_t uid[6], uint16_t board, uint32_t boot_id)
    : trust(keys), trust_count(count), board_id(board) {
    memcpy(current.uid, uid, 6);
    current.boot_id = boot_id ? boot_id : 1;
}

Result Manager::validate(const uint8_t *p, size_t size, Identity &id) const {
    return verify(p, size, trust, trust_count, current.uid, board_id, id);
}

bool Manager::write_verified(Storage &storage, const char *key, const uint8_t *p) {
    if (!storage.write(key, p, PACKAGE_SIZE)) return false;
    uint8_t readback[PACKAGE_SIZE]; size_t length=0;
    return storage.read(key, readback, sizeof(readback), length) == Storage::Read::OK &&
           length == PACKAGE_SIZE && memcmp(p, readback, PACKAGE_SIZE) == 0;
}

void Manager::boot(Storage &storage, bool cold) {
    current.identity = Identity{};
    current.state = State::UNACTIVATED;
    current.pending_revision = 0;
    current.result = Result::OK;
    active_slot = -1;
    bool damaged = false;
    for (int i=0; i<2; ++i) {
        uint8_t bytes[PACKAGE_SIZE]; size_t length=0; Identity id;
        const auto result = storage.read(slots[i], bytes, sizeof(bytes), length);
        if (result == Storage::Read::MISSING) continue;
        if (result != Storage::Read::OK || validate(bytes, length, id) != Result::OK) { damaged=true; continue; }
        if (active_slot >= 0 && id.revision == current.identity.revision && memcmp(bytes, active_package, PACKAGE_SIZE)) {
            current.identity = Identity{}; current.state = State::INVALID; current.result = Result::VERSION_CONFLICT;
            active_slot = -1;
            return;
        }
        if (active_slot < 0 || id.revision > current.identity.revision) {
            current.identity = id; active_slot = i; memcpy(active_package, bytes, PACKAGE_SIZE);
        }
    }
    if (active_slot >= 0) current.state = State::ACTIVE;
    else if (damaged) { current.state = State::INVALID; current.result = Result::STORAGE_ERROR; }

    uint8_t pending[PACKAGE_SIZE]; size_t length=0; Identity id;
    const auto read = storage.read("pending", pending, sizeof(pending), length);
    if (read == Storage::Read::MISSING) return;
    if (read != Storage::Read::OK) { current.result = Result::STORAGE_ERROR; return; }
    current.result = validate(pending, length, id);
    if (current.result != Result::OK) return;
    if (id.revision < current.identity.revision) { current.result=Result::OLD_REVISION; return; }
    if (id.revision == current.identity.revision) {
        if (memcmp(pending, active_package, PACKAGE_SIZE)) { current.result=Result::VERSION_CONFLICT; return; }
        current.result = storage.erase("pending") ? Result::ALREADY_ACTIVE : Result::STORAGE_ERROR;
        return;
    }
    current.pending_revision = id.revision;
    current.result = Result::PENDING_REBOOT;
    if (!cold) return;
    const int next = active_slot == 0 ? 1 : 0;
    if (!write_verified(storage, slots[next], pending)) { current.result=Result::STORAGE_ERROR; return; }
    current.identity=id; current.state=State::ACTIVE; active_slot=next;
    memcpy(active_package, pending, PACKAGE_SIZE);
    current.pending_revision=0;
    current.result=storage.erase("pending") ? Result::OK : Result::STORAGE_ERROR;
}

Result Manager::stage(Storage &storage, const uint8_t *p, size_t size) {
    Identity id;
    Result r = validate(p, size, id);
    if (r != Result::OK) return current.result=r;
    if (id.revision < current.identity.revision || id.revision < current.pending_revision)
        return current.result=Result::OLD_REVISION;
    if (id.revision == current.identity.revision) {
        return current.result = memcmp(p, active_package, PACKAGE_SIZE) == 0 ? Result::ALREADY_ACTIVE : Result::VERSION_CONFLICT;
    }
    if (id.revision == current.pending_revision) {
        uint8_t old[PACKAGE_SIZE]; size_t length=0;
        if (storage.read("pending", old, sizeof(old), length) != Storage::Read::OK || length != PACKAGE_SIZE)
            return current.result=Result::STORAGE_ERROR;
        return current.result = memcmp(p, old, PACKAGE_SIZE) == 0 ? Result::PENDING_REBOOT : Result::VERSION_CONFLICT;
    }
    if (!write_verified(storage, "pending", p)) return current.result=Result::STORAGE_ERROR;
    current.pending_revision=id.revision;
    return current.result=Result::PENDING_REBOOT;
}

bool get_parameter(const Snapshot &s, const char *name, ParamValue &v) {
    v = ParamValue{};
    if (!strcmp(name,"RID_ID_SCHEMA")) v.integer=1;
    else if (!strcmp(name,"RID_ID_REV")) v.integer=s.identity.revision;
    else if (!strcmp(name,"RID_BOOT_ID")) v.integer=s.boot_id;
    else if (!strcmp(name,"RID_ACT_STATE")) v.integer=uint8_t(s.state);
    else if (!strcmp(name,"RID_CLASS")) v.integer=s.identity.aircraft_class;
    else if (!strcmp(name,"RID_PENDING")) v.integer=s.pending_revision;
    else if (!strcmp(name,"RID_ACT_ERR")) v.integer=uint8_t(s.result);
    else {
        v.is_string=true;
        const char *text=nullptr;
        if (!strcmp(name,"RID_UAS_ID")) text=s.identity.product;
        else if (!strcmp(name,"RID_REG_ID")) text=s.identity.registration;
        else if (!strcmp(name,"RID_UIN")) text=s.identity.uin;
        else if (!strcmp(name,"RID_ID_HASH")) text=s.identity.hash;
        else if (!strcmp(name,"RID_UID")) { hex_encode(s.uid,6,v.string); return true; }
        else return false;
        memcpy(v.string,text,strlen(text)+1);
    }
    return true;
}

bool protected_parameter(const char *name) {
    ParamValue value; Snapshot empty;
    return get_parameter(empty,name,value) || !strcmp(name,"CN_UAS_ID") || !strcmp(name,"CN_UIN") || !strcmp(name,"CN_CLASS");
}

const char *result_name(Result r) {
    switch(r) {
    case Result::OK:return "OK";
    case Result::PENDING_REBOOT:return "PENDING_REBOOT";
    case Result::ALREADY_ACTIVE:return "ALREADY_ACTIVE";
    case Result::INVALID_FORMAT:return "INVALID_FORMAT";
    case Result::NO_TRUST:return "NO_TRUST";
    case Result::INVALID_SIGNATURE:return "INVALID_SIGNATURE";
    case Result::WRONG_DEVICE:return "WRONG_DEVICE";
    case Result::INVALID_IDENTITY:return "INVALID_IDENTITY";
    case Result::OLD_REVISION:return "OLD_REVISION";
    case Result::VERSION_CONFLICT:return "VERSION_CONFLICT";
    case Result::STORAGE_ERROR:return "STORAGE_ERROR";
    case Result::BUSY:return "BUSY";
    }
    return "UNKNOWN";
}
}
