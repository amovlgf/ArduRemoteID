#include "activation_service.h"
#include "activation_trust.h"
#include "board_config.h"
#include "parameters.h"
#include <esp_system.h>
#include <nvs.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <new>

namespace ActivationService {
using namespace rid_activation;
namespace {
constexpr const char *NVS_NAMESPACE="rid_activation";
class NVSStorage : public Storage {
public:
    bool open() { return nvs_open(NVS_NAMESPACE,NVS_READWRITE,&handle)==ESP_OK; }
    Read read(const char *key,uint8_t *bytes,size_t capacity,size_t &size) override {
        size=capacity;
        const esp_err_t ret=nvs_get_blob(handle,key,bytes,&size);
        return ret==ESP_OK ? Read::OK : ret==ESP_ERR_NVS_NOT_FOUND ? Read::MISSING : Read::ERROR;
    }
    bool write(const char *key,const uint8_t *bytes,size_t size) override {
        return nvs_set_blob(handle,key,bytes,size)==ESP_OK && nvs_commit(handle)==ESP_OK;
    }
    bool erase(const char *key) override {
        const esp_err_t ret=nvs_erase_key(handle,key);
        return (ret==ESP_OK || ret==ESP_ERR_NVS_NOT_FOUND) && nvs_commit(handle)==ESP_OK;
    }
private:
    nvs_handle handle=0;
};
NVSStorage storage;
alignas(Manager) uint8_t manager_memory[sizeof(Manager)];
Manager *manager=nullptr;
Snapshot published;
portMUX_TYPE snapshot_mux=portMUX_INITIALIZER_UNLOCKED;
SemaphoreHandle_t storage_mutex=nullptr;
QueueHandle_t requests=nullptr;
struct Request { uint8_t bytes[PACKAGE_SIZE]; };
bool outstanding=false;
bool stopping=false;
bool ready=false;
void publish() {
    portENTER_CRITICAL(&snapshot_mux);
    published=manager->snapshot();
    portEXIT_CRITICAL(&snapshot_mux);
}
String quoted(const char *s) {
    String result="\"";
    for (; *s; ++s) { if (*s=='"' || *s=='\\') result+='\\'; result+=*s; }
    return result+'"';
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
    manager=new(manager_memory) Manager(ActivationTrust::keys,ActivationTrust::count,uid,BOARD_ID,esp_random());
    storage_mutex=xSemaphoreCreateMutex();
    requests=xQueueCreate(1,sizeof(Request));
    ready=storage_mutex && requests && storage.open();
    if (ready) manager->boot(storage,esp_reset_reason()==ESP_RST_POWERON);
    publish();
    if (!ready) {
        published.state=State::INVALID;
        published.result=Result::STORAGE_ERROR;
    }
    if (required()) {
        const auto s=snapshot();
        memcpy(g.cn_uas_id,s.identity.product,sizeof(s.identity.product));
        memcpy(g.cn_uin,s.identity.uin,sizeof(s.identity.uin));
        g.cn_class=s.identity.aircraft_class;
    }
}
Snapshot snapshot() {
    portENTER_CRITICAL(&snapshot_mux);
    const Snapshot copy=published;
    portEXIT_CRITICAL(&snapshot_mux);
    return copy;
}
bool business_allowed() { return !required() || snapshot().state==State::ACTIVE; }
const char *failure() {
    if (!required()) return nullptr;
    const auto s=snapshot();
    if (s.state==State::ACTIVE) return nullptr;
    return s.state==State::INVALID ? "RID activation invalid" : "RID not activated";
}
bool submit(const uint8_t *bytes,size_t size) {
    if (!ready || size!=PACKAGE_SIZE) return false;
    portENTER_CRITICAL(&snapshot_mux);
    const bool busy=outstanding || stopping;
    if (!busy) outstanding=true;
    portEXIT_CRITICAL(&snapshot_mux);
    if (busy) return false;
    Request request;
    memcpy(request.bytes,bytes,sizeof(request.bytes));
    if (xQueueSend(requests,&request,0)==pdTRUE) return true;
    portENTER_CRITICAL(&snapshot_mux); outstanding=false; portEXIT_CRITICAL(&snapshot_mux);
    return false;
}
void process() {
    Request request;
    if (!ready || xQueueReceive(requests,&request,0)!=pdTRUE) return;
    xSemaphoreTake(storage_mutex,portMAX_DELAY);
    if (!stopping) { manager->stage(storage,request.bytes,sizeof(request.bytes)); publish(); }
    xSemaphoreGive(storage_mutex);
    portENTER_CRITICAL(&snapshot_mux); outstanding=false; portEXIT_CRITICAL(&snapshot_mux);
}
bool clear_pending_for_defaults() {
    if (storage_mutex) xSemaphoreTake(storage_mutex,portMAX_DELAY);
    portENTER_CRITICAL(&snapshot_mux); stopping=true; portEXIT_CRITICAL(&snapshot_mux);
    nvs_handle reset_handle=0;
    bool ok=nvs_open(NVS_NAMESPACE,NVS_READWRITE,&reset_handle)==ESP_OK;
    if (ok) {
        const esp_err_t erased=nvs_erase_key(reset_handle,"pending");
        ok=(erased==ESP_OK || erased==ESP_ERR_NVS_NOT_FOUND) && nvs_commit(reset_handle)==ESP_OK;
        nvs_close(reset_handle);
    }
    if (storage_mutex) xSemaphoreGive(storage_mutex);
    return ok;
}
String status_json() {
    const auto s=snapshot();
    portENTER_CRITICAL(&snapshot_mux); const bool busy=outstanding; portEXIT_CRITICAL(&snapshot_mux);
    char uid[13]; hex_encode(s.uid,6,uid);
    String json="{\"schema\":1,\"uid\":"+quoted(uid);
    json+=",\"boot_id\":"+String(s.boot_id)+",\"revision\":"+String(s.identity.revision);
    json+=",\"state\":"+String(uint8_t(s.state))+",\"pending\":"+String(s.pending_revision);
    json+=",\"result\":"+String(uint8_t(s.result))+",\"result_name\":"+quoted(result_name(s.result));
    json+=",\"busy\":"+String(busy?"true":"false")+",\"test_build\":"+String(ActivationTrust::test_build?"true":"false");
    json+=",\"trust_roots\":"+String(ActivationTrust::count);
    // ESP-IDF reports the minimum free stack in bytes (not vanilla RTOS words).
    json+=",\"web_stack_min_free_bytes\":"+String(uxTaskGetStackHighWaterMark(nullptr));
    json+=",\"free_heap_bytes\":"+String(ESP.getFreeHeap());
    json+=",\"uas_id\":"+quoted(s.identity.product)+",\"registration\":"+quoted(s.identity.registration);
    json+=",\"uin\":"+quoted(s.identity.uin)+",\"class\":"+String(s.identity.aircraft_class);
    json+=",\"hash\":"+quoted(s.identity.hash)+"}";
    return json;
}
}
