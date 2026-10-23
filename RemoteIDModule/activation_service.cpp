#include "activation_service.h"
#include "board_config.h"
#include "parameters.h"
#include <esp_system.h>
#include <nvs.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <new>

namespace ActivationService {
using namespace rid_activation;
namespace {
class NVSStorage : public Storage {
public:
    bool open() { opened=nvs_open("rid_platform", NVS_READWRITE, &handle)==ESP_OK; return opened; }
    Read read(const char *key, uint8_t *bytes, size_t capacity, size_t &size) override {
        if (!opened) return Read::ERROR;
        size = capacity;
        const esp_err_t ret = nvs_get_blob(handle, key, bytes, &size);
        return ret == ESP_OK ? Read::OK : ret == ESP_ERR_NVS_NOT_FOUND ? Read::MISSING : Read::ERROR;
    }
    bool write(const char *key, const uint8_t *bytes, size_t size) override {
        if (!opened) return false;
        if (nvs_set_blob(handle, key, bytes, size)==ESP_OK && nvs_commit(handle)==ESP_OK) return true;
        // Discard any uncommitted handle state before reconciliation reads.
        nvs_close(handle);
        open();
        return false;
    }
    bool erase(const char *key) override {
        const esp_err_t ret = nvs_erase_key(handle, key);
        return (ret == ESP_OK || ret == ESP_ERR_NVS_NOT_FOUND) && nvs_commit(handle) == ESP_OK;
    }
private:
    nvs_handle handle = 0;
    bool opened = false;
};
NVSStorage storage;
alignas(platform_activation::Manager) uint8_t platform_memory[sizeof(platform_activation::Manager)];
platform_activation::Manager *platform = nullptr;
platform_activation::State published;
portMUX_TYPE snapshot_mux = portMUX_INITIALIZER_UNLOCKED;
SemaphoreHandle_t storage_mutex = nullptr;
bool ready = false;

void publish(const platform_activation::State &state) {
    portENTER_CRITICAL(&snapshot_mux);
    published = state;
    portEXIT_CRITICAL(&snapshot_mux);
}

String quoted(const char *s) {
    String result = "\"";
    for (; *s; ++s) { if (*s == '"' || *s == '\\') result += '\\'; result += *s; }
    return result + '"';
}
}

bool required() {
#ifdef BOARD_AMOV_RID
    return true;
#else
    return false;
#endif
}

void init() {
    uint8_t uid[6] {};
    esp_efuse_mac_get_default(uid);
    platform_activation::State initial;
    memcpy(initial.snapshot.uid, uid, sizeof(uid));
    const uint32_t boot = esp_random();
    initial.snapshot.boot_id = boot ? boot : 1;
    if (required()) {
        platform = new(platform_memory) platform_activation::Manager(uid, initial.snapshot.boot_id);
        storage_mutex = xSemaphoreCreateMutex();
        ready = storage_mutex && storage.open();
        if (ready) {
            platform->boot(storage);
            initial = platform->state();
        } else {
            initial.snapshot.state = State::INVALID;
            initial.snapshot.result = Result::STORAGE_ERROR;
        }
    }
    publish(initial);
    if (required()) {
        const auto s = snapshot();
        memcpy(g.cn_uas_id, s.identity.product, sizeof(s.identity.product));
        memcpy(g.cn_uin, s.identity.uin, sizeof(s.identity.uin));
        g.cn_class = s.identity.aircraft_class;
    }
}

platform_activation::State platform_state() {
    portENTER_CRITICAL(&snapshot_mux);
    const auto copy = published;
    portEXIT_CRITICAL(&snapshot_mux);
    return copy;
}

Snapshot snapshot() { return platform_state().snapshot; }
bool factory_provision_available() {
    return required() && ready && g.lock_level==0 && platform_state().provision_available;
}
bool business_allowed() { return !required() || snapshot().state == State::ACTIVE; }

const char *failure() {
    if (!required()) return nullptr;
    const auto state = platform_state();
    if (state.snapshot.state == State::INVALID) return "RID activation invalid";
    if (!state.provisioned) return "RID identity not provisioned";
    return state.snapshot.state == State::ACTIVE ? nullptr : "RID not activated";
}

Result synchronize(const char sn[20], bool active, uint64_t transaction) {
    if (!ready || !platform) return Result::STORAGE_ERROR;
    xSemaphoreTake(storage_mutex, portMAX_DELAY);
    const auto result = platform->synchronize(storage, sn, active, transaction);
    publish(platform->state());
    xSemaphoreGive(storage_mutex);
    return result;
}

Result provision(const char sn[20], uint8_t aircraft_class) {
    if (!required()) return Result::NO_TRUST;
    if (!ready || !platform) return Result::STORAGE_ERROR;
    xSemaphoreTake(storage_mutex, portMAX_DELAY);
    const auto result=g.lock_level==0 ? platform->provision(storage,sn,aircraft_class) : Result::NO_TRUST;
    publish(platform->state());
    xSemaphoreGive(storage_mutex);
    return result;
}

String status_json() {
    const auto state = platform_state();
    const auto &s = state.snapshot;
    char uid[13];
    hex_encode(s.uid, 6, uid);
    String json = "{\"schema\":1,\"uid\":" + quoted(uid);
    json += ",\"boot_id\":" + String(s.boot_id) + ",\"revision\":" + String(s.identity.revision);
    json += ",\"state\":" + String(uint8_t(s.state)) + ",\"pending\":0";
    json += ",\"result\":" + String(uint8_t(s.result)) + ",\"result_name\":" + quoted(result_name(s.result));
    json += ",\"required\":" + String(required() ? "true" : "false");
    json += ",\"activation_method\":" + quoted(required() ? "platform" : "none");
    json += ",\"provisioned\":" + String(state.provisioned ? "true" : "false");
    json += ",\"factory_provision_available\":" + String(required() && ready && g.lock_level==0 && state.provision_available ? "true" : "false");
    json += ",\"activation_source\":" + String(uint8_t(state.source)) + ",\"activation_generation\":" + String(state.generation);
    // ESP-IDF reports the minimum free stack in bytes, unlike vanilla FreeRTOS.
    json += ",\"web_stack_min_free_bytes\":" + String(uxTaskGetStackHighWaterMark(nullptr));
    json += ",\"free_heap_bytes\":" + String(ESP.getFreeHeap());
    json += ",\"uas_id\":" + quoted(s.identity.product) + ",\"registration\":" + quoted(s.identity.registration);
    json += ",\"uin\":" + quoted(s.identity.uin) + ",\"class\":" + String(s.identity.aircraft_class);
    json += ",\"hash\":" + quoted(s.identity.hash) + "}";
    return json;
}
}
