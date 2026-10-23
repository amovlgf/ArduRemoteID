#include "RemoteIDModule/activation_service.h"
#include "RemoteIDModule/parameters.h"
#include <nvs.h>
#include <freertos/semphr.h>
#include <cassert>
#include <map>
#include <string>
#include <vector>

Parameters g;
FakeESP ESP;
namespace {
const uint8_t uid[6] = {0x10,0x00,0x3b,0xd1,0xf3,0x10};
const char product[21] = "AMOV0123456789012345";
std::map<std::string,std::vector<uint8_t>> data, pending;
std::string fault_key;
bool set_error=false, commit_error=false, commit_then_fail=false, fail_read_after_write=false;
unsigned opens=0, reads=0, writes=0, commits=0, mutexes=0, closes=0;
bool open_error=false, read_error=false, mutex_error=false;
int mutex;

void provision() {
    rid_activation::Identity identity;
    identity.revision=7;
    memcpy(identity.uid,uid,6);
    strcpy(identity.product,product);
    strcpy(identity.registration,"REG-12345678");
    strcpy(identity.uin,"12345678");
    identity.aircraft_class=2;
    uint8_t bytes[platform_activation::FACTORY_SIZE];
    platform_activation::encode_factory(identity,platform_activation::Source::FACTORY,bytes);
    data["factory"]={bytes,bytes+sizeof(bytes)};
}
void contains(const String &json,const char *text) { assert(strstr(json.c_str(),text)); }
}

void esp_efuse_mac_get_default(uint8_t *out) { memcpy(out,uid,6); }
uint32_t esp_random() { return 0; } // The service must replace an invalid zero boot ID.
SemaphoreHandle_t xSemaphoreCreateMutex() { ++mutexes; return mutex_error?nullptr:&mutex; }
int xSemaphoreTake(SemaphoreHandle_t handle,uint32_t) { assert(handle==&mutex); return 1; }
int xSemaphoreGive(SemaphoreHandle_t handle) { assert(handle==&mutex); return 1; }
esp_err_t nvs_open(const char *name,int mode,nvs_handle *handle) {
    ++opens;
    assert(!strcmp(name,"rid_platform") && mode==NVS_READWRITE);
    *handle=1;
    return open_error?ESP_FAIL:ESP_OK;
}
void nvs_close(nvs_handle handle) { assert(handle==1); ++closes; pending.clear(); }
esp_err_t nvs_get_blob(nvs_handle handle,const char *key,void *out,size_t *size) {
    assert(handle==1); ++reads;
    if(read_error || (fail_read_after_write && fault_key==key && data.count(key))) return ESP_FAIL;
    const auto it=data.find(key);
    if(it==data.end()) return ESP_ERR_NVS_NOT_FOUND;
    if(*size<it->second.size()) return ESP_FAIL;
    *size=it->second.size(); memcpy(out,it->second.data(),*size); return ESP_OK;
}
esp_err_t nvs_set_blob(nvs_handle handle,const char *key,const void *in,size_t size) {
    assert(handle==1); ++writes;
    const auto *p=static_cast<const uint8_t *>(in);
    assert(!strcmp(key,"factory") || !strcmp(key,"state_a") || !strcmp(key,"state_b") || !strcmp(key,"ready"));
    if(set_error && fault_key==key) return ESP_FAIL;
    pending[key]={p,p+size}; return ESP_OK;
}
esp_err_t nvs_erase_key(nvs_handle,const char *) { assert(false); return ESP_FAIL; }
esp_err_t nvs_commit(nvs_handle handle) {
    assert(handle==1); ++commits;
    const bool fail=commit_error && pending.count(fault_key);
    if (fail && !commit_then_fail) return ESP_FAIL;
    for (const auto &record : pending) data[record.first]=record.second;
    pending.clear();
    return fail?ESP_FAIL:ESP_OK;
}

int main(int argc,char **argv) {
    assert(argc==2);
    const std::string scenario=argv[1];
    strcpy(g.cn_uas_id,"existing-non-amov-id");
    strcpy(g.cn_uin,"87654321");
    g.cn_class=4;
    if(scenario=="provisioned") provision();
    if(scenario=="open_error") open_error=true;
    if(scenario=="read_error") read_error=true;
    if(scenario=="mutex_error") mutex_error=true;
    ActivationService::init();
    assert(!memcmp(ActivationService::snapshot().uid,uid,6));
    assert(ActivationService::snapshot().boot_id==1);
#ifndef BOARD_AMOV_RID
    assert(!ActivationService::required());
    assert(ActivationService::business_allowed());
    assert(ActivationService::failure()==nullptr);
    assert(!opens && !reads && !writes && !commits && !mutexes);
    assert(!strcmp(g.cn_uas_id,"existing-non-amov-id") && !strcmp(g.cn_uin,"87654321") && g.cn_class==4);
    assert(ActivationService::synchronize(product,true,123)==rid_activation::Result::STORAGE_ERROR);
    assert(ActivationService::provision(product,2)==rid_activation::Result::NO_TRUST);
    assert(!ActivationService::factory_provision_available());
    assert(!opens && !reads && !writes);
    contains(ActivationService::status_json(),"\"required\":false");
    contains(ActivationService::status_json(),"\"activation_method\":\"none\"");
#else
    assert(ActivationService::required());
    assert(!ActivationService::business_allowed());
    assert(mutexes==1);
    contains(ActivationService::status_json(),"\"required\":true");
    contains(ActivationService::status_json(),"\"activation_method\":\"platform\"");
    if(scenario=="provisioned") {
        assert(ActivationService::platform_state().provisioned);
        assert(!strcmp(g.cn_uas_id,product) && !strcmp(g.cn_uin,"12345678") && g.cn_class==2);
        assert(!strcmp(ActivationService::failure(),"RID not activated"));
        assert(ActivationService::synchronize("BADV0123456789012345",true,123)==rid_activation::Result::WRONG_DEVICE);
        assert(ActivationService::synchronize(product,true,123)==rid_activation::Result::OK);
        assert(ActivationService::business_allowed() && !ActivationService::failure());
        ActivationService::init();
        assert(ActivationService::business_allowed());
        assert(ActivationService::snapshot().identity.revision==7);
        contains(ActivationService::status_json(),"\"uas_id\":\"AMOV0123456789012345\"");
        assert(ActivationService::synchronize(product,false,124)==rid_activation::Result::OK);
        ActivationService::init();
        assert(!ActivationService::business_allowed());
    } else if(scenario=="missing") {
        assert(!strcmp(ActivationService::failure(),"RID identity not provisioned"));
        assert(!ActivationService::platform_state().provisioned);
        assert(ActivationService::synchronize(product,true,123)==rid_activation::Result::WRONG_DEVICE);
        assert(!writes && !commits && reads==4);
        assert(ActivationService::factory_provision_available());
        assert(g.cn_uas_id[0]==0 && g.cn_uin[0]==0 && g.cn_class==0);
    } else if(scenario=="first_provision") {
        using rid_activation::Result;
        assert(ActivationService::factory_provision_available());
        assert(ActivationService::provision("AMOV0123456789012345",5)==Result::INVALID_IDENTITY);
        assert(!writes && !commits);
        for (int8_t level : {-1,1,2}) {
            g.lock_level=level;
            assert(!ActivationService::factory_provision_available());
            assert(ActivationService::provision(product,2)==Result::NO_TRUST);
            assert(!writes && !commits);
        }
        g.lock_level=0;
        assert(ActivationService::provision(product,2)==Result::OK);
        assert(!ActivationService::factory_provision_available());
        const auto current=ActivationService::platform_state();
        assert(current.provisioned && !current.provision_available);
        assert(current.snapshot.state==rid_activation::State::UNACTIVATED);
        assert(current.snapshot.identity.revision==1 && current.snapshot.identity.aircraft_class==2);
        assert(!strcmp(current.snapshot.identity.product,product));
        assert(!current.snapshot.identity.registration[0] && !current.snapshot.identity.uin[0]);
        assert(!ActivationService::business_allowed());
        // The worker publishes an immutable snapshot. Main-task management
        // applies the g.cn_* mirror, avoiding a network/main-task data race.
        assert(g.cn_uas_id[0]==0 && g.cn_uin[0]==0 && g.cn_class==0);
        contains(ActivationService::status_json(),"\"provisioned\":true");
        contains(ActivationService::status_json(),"\"factory_provision_available\":false");
        const auto records=data; const auto count=writes;
        assert(ActivationService::provision("DIFF0123456789012345",4)==Result::VERSION_CONFLICT);
        assert(data==records && writes==count);
        ActivationService::init();
        assert(!strcmp(g.cn_uas_id,product) && g.cn_class==2 && !g.cn_uin[0]);
        assert(!ActivationService::business_allowed());
        assert(ActivationService::synchronize(product,true,123)==Result::OK);
        ActivationService::init();
        assert(ActivationService::business_allowed());
    } else if(scenario=="provision_errors") {
        using rid_activation::Result;
        for(const char *key : {"factory","state_a","ready"}) {
            for(unsigned fault=0; fault<4; ++fault) {
                data.clear(); pending.clear(); fault_key.clear();
                set_error=commit_error=commit_then_fail=fail_read_after_write=false;
                ActivationService::init();
                assert(ActivationService::factory_provision_available());
                const auto old_closes=closes;
                fault_key=key;
                set_error=fault==0; commit_error=fault==1 || fault==2;
                commit_then_fail=fault==2; fail_read_after_write=fault==3;
                assert(ActivationService::provision(product,2)==Result::STORAGE_ERROR);
                assert(!ActivationService::business_allowed());
                const bool has_factory=data.count("factory");
                assert(ActivationService::factory_provision_available()==!has_factory);
                if(fault!=3) assert(closes>old_closes && pending.empty());
                if(fault==2) {
                    assert(ActivationService::platform_state().provisioned);
                    assert(!strcmp(ActivationService::snapshot().identity.product,product));
                }
                const auto original=has_factory?data.at("factory"):std::vector<uint8_t>{};
                set_error=commit_error=commit_then_fail=fail_read_after_write=false;
                if(has_factory) {
                    assert(ActivationService::provision("DIFF0123456789012345",3)!=Result::OK);
                    assert(data.at("factory")==original);
                } else assert(ActivationService::provision(product,2)==Result::OK);
                ActivationService::init();
                assert(!ActivationService::business_allowed());
                assert(!ActivationService::factory_provision_available());
            }
        }
    } else {
        assert(ActivationService::snapshot().state==rid_activation::State::INVALID);
        assert(ActivationService::snapshot().result==rid_activation::Result::STORAGE_ERROR);
        assert(!strcmp(ActivationService::failure(),"RID activation invalid"));
        assert(!writes && !commits);
        if(scenario=="mutex_error") assert(!opens);
        if(scenario=="open_error" || scenario=="mutex_error") {
            assert(ActivationService::synchronize(product,true,123)==rid_activation::Result::STORAGE_ERROR);
            assert(ActivationService::provision(product,2)==rid_activation::Result::STORAGE_ERROR);
        }
        assert(!ActivationService::factory_provision_available());
    }
#endif
}
