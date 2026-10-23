#pragma once

#include <stddef.h>
#include <stdint.h>

// Shared identity, status and storage types for platform activation.
namespace rid_activation {
// Keep result values stable for the management protocol, including values no
// longer produced by the removed signed-authorization flow.
enum class Result : uint8_t {
    OK = 0, PENDING_REBOOT = 1, ALREADY_ACTIVE = 2, INVALID_FORMAT = 3,
    NO_TRUST = 4, INVALID_SIGNATURE = 5, WRONG_DEVICE = 6,
    INVALID_IDENTITY = 7, OLD_REVISION = 8, VERSION_CONFLICT = 9,
    STORAGE_ERROR = 10, BUSY = 11
};
enum class State : uint8_t { UNACTIVATED = 0, ACTIVE = 1, INVALID = 2 };
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

class Storage {
public:
    enum class Read { MISSING, OK, ERROR };
    virtual Read read(const char *key, uint8_t *bytes, size_t capacity, size_t &size) = 0;
    // Write must commit durably before returning success. The caller reads back.
    virtual bool write(const char *key, const uint8_t *bytes, size_t size) = 0;
    virtual bool erase(const char *key) = 0;
    virtual ~Storage() = default;
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
