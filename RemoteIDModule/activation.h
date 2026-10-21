#pragma once

#include <stddef.h>
#include <stdint.h>

// Hardware-independent activation protocol and transaction implementation.
namespace rid_activation {
constexpr size_t SIGNED_SIZE = 140;
constexpr size_t PACKAGE_SIZE = SIGNED_SIZE + 64;
constexpr size_t MAX_UPLOAD_SIZE = 1024;
constexpr uint16_t TEST_KEY_ID = 65535;

enum class Result : uint8_t {
    OK = 0, PENDING_REBOOT = 1, ALREADY_ACTIVE = 2, INVALID_FORMAT = 3,
    NO_TRUST = 4, INVALID_SIGNATURE = 5, WRONG_DEVICE = 6,
    INVALID_IDENTITY = 7, OLD_REVISION = 8, VERSION_CONFLICT = 9,
    STORAGE_ERROR = 10, BUSY = 11
};
enum class State : uint8_t { UNACTIVATED = 0, ACTIVE = 1, INVALID = 2 };
struct TrustKey { uint16_t id; uint8_t public_key[32]; };
struct Identity {
    uint32_t revision = 0;
    uint8_t uid[6] {};
    uint8_t aircraft_class = 0;
    char product[21] {};
    char registration[65] {};
    char uin[9] {};
    char hash[65] {};
};
struct Snapshot {
    Identity identity;
    uint8_t uid[6] {};
    uint32_t boot_id = 0;
    uint32_t pending_revision = 0;
    State state = State::UNACTIVATED;
    Result result = Result::OK;
};

const char *result_name(Result result);
void hex_encode(const uint8_t *bytes, size_t count, char *output);
void identity_hash(Identity &identity);
Result verify(const uint8_t *package, size_t size, const TrustKey *keys, size_t key_count,
              const uint8_t uid[6], uint16_t board_id, Identity &identity);

class Storage {
public:
    enum class Read { MISSING, OK, ERROR };
    virtual Read read(const char *key, uint8_t *bytes, size_t capacity, size_t &size) = 0;
    // Write must commit durably before returning success. The caller reads back.
    virtual bool write(const char *key, const uint8_t *bytes, size_t size) = 0;
    virtual bool erase(const char *key) = 0;
    virtual ~Storage() = default;
};

class Manager {
public:
    Manager(const TrustKey *keys, size_t count, const uint8_t uid[6], uint16_t board, uint32_t boot);
    void boot(Storage &storage, bool cold_power_on);
    Result stage(Storage &storage, const uint8_t *package, size_t size);
    const Snapshot &snapshot() const { return current; }
private:
    const TrustKey *trust;
    size_t trust_count;
    uint16_t board_id;
    Snapshot current;
    uint8_t active_package[PACKAGE_SIZE] {};
    int active_slot = -1;
    Result validate(const uint8_t *bytes, size_t size, Identity &identity) const;
    bool write_verified(Storage &storage, const char *key, const uint8_t *bytes);
};

struct ParamValue {
    bool is_string = false;
    uint32_t integer = 0;
    char string[65] {};
};
// Returns false for a name outside this read-only namespace.
bool get_parameter(const Snapshot &snapshot, const char *name, ParamValue &value);
bool protected_parameter(const char *name);
}
