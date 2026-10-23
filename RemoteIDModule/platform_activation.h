#pragma once
#include "activation.h"

// Portable factory identity and platform-state storage.
namespace platform_activation {
constexpr size_t FACTORY_SIZE = 120;
constexpr size_t STATE_SIZE = 68;
// IMPORTED preserves the historical on-device value. It never authorizes
// activation or causes an old signed authorization to be read or imported.
enum class Source : uint8_t { FACTORY = 0, IMPORTED = 1, PLATFORM = 2 };
struct State {
    rid_activation::Snapshot snapshot;
    Source source = Source::FACTORY;
    uint32_t generation = 0;
    uint64_t transaction = 0;
    bool provisioned = false;
    // True only after every identity/state key was confirmed absent.
    bool provision_available = false;
};
uint32_t crc32(const uint8_t *bytes, size_t size);
bool decode_factory(const uint8_t *bytes, size_t size, const uint8_t uid[6],
                    rid_activation::Identity &identity, Source &source);
void encode_factory(const rid_activation::Identity &identity, Source source, uint8_t out[FACTORY_SIZE]);
class Manager {
public:
    Manager(const uint8_t uid[6], uint32_t boot);
    void boot(rid_activation::Storage &storage);
    rid_activation::Result provision(rid_activation::Storage &storage, const char sn[20],
                                     uint8_t aircraft_class);
    rid_activation::Result synchronize(rid_activation::Storage &storage, const char sn[20], bool active,
                                       uint64_t transaction);
    const State &state() const { return current; }
private:
    State current;
    int slot = -1;
    bool save(rid_activation::Storage &storage, const State &next);
};
}
