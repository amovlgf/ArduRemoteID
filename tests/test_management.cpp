#include "RemoteIDModule/management_service.h"
#include "RemoteIDModule/management_parameters.h"
#include "RemoteIDModule/activation_service.h"
#include "RemoteIDModule/romfs.h"
#include "RemoteIDModule/monocypher.h"
#include "RemoteIDModule/parameter_text.h"
#include <freertos/queue.h>
#include <nvs.h>
#include <algorithm>
#include <cassert>
#include <cmath>
#include <iostream>
#include <fstream>
#include <iterator>
#include <limits>
#include <map>
#include <string>
#include <vector>
using namespace amov_device;

FakeSerial Serial;
static uint32_t clock_ms=100;
static unsigned restarts=0,erases=0,write_calls=0;
uint32_t millis() { return clock_ms; }
void esp_restart() { ++restarts; }
void esp_read_mac(uint8_t *uid,int) { const uint8_t value[]={0xaa,0xbb,0xcc,0xdd,0xee,0xff}; memcpy(uid,value,6); }
const char *ROMFS::find_string(const char *path) {
    static std::map<std::string,std::string> files;
    auto &contents=files[path];
    if(contents.empty()) {
        std::ifstream input(std::string("RemoteIDModule/")+path,std::ios::binary);
        contents.assign(std::istreambuf_iterator<char>(input),{});
        assert(input || !contents.empty());
    }
    return contents.c_str();
}
int32_t base64_decode(const char *,uint8_t *,uint32_t) { return -1; }
char *base64_encode(const uint8_t *,int) { return nullptr; }
struct FakeQueue { size_t size; bool occupied=false; std::vector<uint8_t> data; };
QueueHandle_t xQueueCreate(unsigned,size_t size) { auto *queue=new FakeQueue; queue->size=size; queue->data.resize(size); return queue; }
void vQueueDelete(QueueHandle_t queue) { delete queue; }
int xQueueSend(QueueHandle_t queue,const void *data,unsigned) { if(queue->occupied) return 0; memcpy(queue->data.data(),data,queue->size); queue->occupied=true; return 1; }
int xQueueReceive(QueueHandle_t queue,void *data,unsigned) { if(!queue->occupied) return 0; memcpy(data,queue->data.data(),queue->size); queue->occupied=false; return 1; }

struct Value { unsigned type; std::vector<uint8_t> bytes; };
struct Handle { std::string name; std::map<std::string,Value> pending; bool erase=false; };
static std::map<std::string,std::map<std::string,Value>> database;
static std::map<unsigned,Handle> handles;
static unsigned next_handle=0;
static std::string failure_namespace;
static unsigned fail_commit_at=0,commits=0,fail_read_at=0,reads=0;
static bool fail_set=false,commit_then_fail=false;
esp_err_t nvs_open(const char *name,int,nvs_handle *handle) { *handle=++next_handle; handles[*handle].name=name; return ESP_OK; }
void nvs_close(nvs_handle handle) { handles.erase(handle); }
esp_err_t nvs_commit(nvs_handle handle) {
    auto &entry=handles.at(handle); const bool fail=entry.name==failure_namespace && ++commits==fail_commit_at;
    if(fail && !commit_then_fail) return ESP_FAIL;
    if(entry.erase) { database[entry.name].clear(); entry.erase=false; }
    for(const auto &item:entry.pending) database[entry.name][item.first]=item.second;
    entry.pending.clear(); return fail?ESP_FAIL:ESP_OK;
}
esp_err_t nvs_erase_all(nvs_handle handle) { ++erases; handles.at(handle).erase=true; handles.at(handle).pending.clear(); return ESP_OK; }
esp_err_t nvs_erase_key(nvs_handle handle,const char *key) { database[handles.at(handle).name].erase(key); return ESP_OK; }
static esp_err_t set(nvs_handle handle,const char *key,unsigned type,const void *value,size_t size) {
    ++write_calls; auto &entry=handles.at(handle);
    if(fail_set && entry.name==failure_namespace) { fail_set=false; return ESP_FAIL; }
    const auto *bytes=static_cast<const uint8_t *>(value); entry.pending[key]={type,std::vector<uint8_t>(bytes,bytes+size)}; return ESP_OK;
}
static esp_err_t get(nvs_handle handle,const char *key,unsigned type,void *value,size_t *size) {
    const auto &entry=handles.at(handle);
    if(entry.name==failure_namespace && ++reads==fail_read_at) return ESP_FAIL;
    auto &values=database[entry.name]; const auto it=values.find(key); if(it==values.end()) return ESP_ERR_NVS_NOT_FOUND;
    if(it->second.type!=type) return ESP_ERR_NVS_TYPE_MISMATCH;
    if(*size<it->second.bytes.size()) return ESP_FAIL;
    *size=it->second.bytes.size(); memcpy(value,it->second.bytes.data(),*size); return ESP_OK;
}
esp_err_t nvs_set_u8(nvs_handle h,const char *k,uint8_t v) { return set(h,k,1,&v,1); }
esp_err_t nvs_set_i8(nvs_handle h,const char *k,int8_t v) { return set(h,k,2,&v,1); }
esp_err_t nvs_set_u32(nvs_handle h,const char *k,uint32_t v) { return set(h,k,3,&v,4); }
esp_err_t nvs_set_str(nvs_handle h,const char *k,const char *v) { return set(h,k,4,v,strlen(v)+1); }
esp_err_t nvs_set_blob(nvs_handle h,const char *k,const void *v,size_t s) { return set(h,k,5,v,s); }
esp_err_t nvs_get_u8(nvs_handle h,const char *k,uint8_t *v) { size_t s=1; return get(h,k,1,v,&s); }
esp_err_t nvs_get_i8(nvs_handle h,const char *k,int8_t *v) { size_t s=1; return get(h,k,2,v,&s); }
esp_err_t nvs_get_u32(nvs_handle h,const char *k,uint32_t *v) { size_t s=4; return get(h,k,3,v,&s); }
esp_err_t nvs_get_str(nvs_handle h,const char *k,char *v,size_t *s) { return get(h,k,4,v,s); }
esp_err_t nvs_get_blob(nvs_handle h,const char *k,void *v,size_t *s) { return get(h,k,5,v,s); }
static platform_activation::State activation;
static unsigned provision_calls=0;
static bool provision_uncertain=false;
namespace ActivationService {
bool required() { return true; }
rid_activation::Snapshot snapshot() { return activation.snapshot; }
platform_activation::State platform_state() { return activation; }
bool factory_provision_available() {
    return g.lock_level==0 && activation.provision_available && !activation.provisioned &&
        activation.snapshot.state==rid_activation::State::UNACTIVATED;
}
rid_activation::Result provision(const char sn[20],uint8_t aircraft_class) {
    ++provision_calls;
    if(g.lock_level!=0) return rid_activation::Result::NO_TRUST;
    if(!factory_provision_available()) return rid_activation::Result::VERSION_CONFLICT;
    auto &identity=activation.snapshot.identity;
    memcpy(identity.uid,activation.snapshot.uid,6); memcpy(identity.product,sn,20);
    identity.aircraft_class=aircraft_class; identity.revision=1;
    rid_activation::identity_hash(identity);
    activation.provisioned=true; activation.provision_available=false; activation.generation=1;
    return provision_uncertain?rid_activation::Result::STORAGE_ERROR:rid_activation::Result::OK;
}
rid_activation::Result synchronize(const char sn[20],bool active,uint64_t transaction) {
    if(memcmp(sn,activation.snapshot.identity.product,20)) return rid_activation::Result::WRONG_DEVICE;
    activation.snapshot.state=active?rid_activation::State::ACTIVE:rid_activation::State::UNACTIVATED;
    activation.source=platform_activation::Source::PLATFORM; activation.transaction=transaction; ++activation.generation;
    return rid_activation::Result::OK;
}
}
static void fault_clear() { failure_namespace.clear(); fail_commit_at=commits=fail_read_at=reads=0; fail_set=false; commit_then_fail=false; }
static void boot() {
    fault_clear(); ++activation.snapshot.boot_id;
    g=Parameters{}; g.init(); g.rid_profile=1; ManagementService::init();
}
static Request request(Operation operation) {
    static uint32_t next=100; Request r; r.operation=operation; r.transaction=++next; r.boot=activation.snapshot.boot_id; memcpy(r.uid,activation.snapshot.uid,6); return r;
}
static Response call(const Request &r) {
    uint8_t bytes[REQUEST_SIZE],out[RESPONSE_SIZE]; assert(encode_request(r,bytes,sizeof(bytes)));
    assert(ManagementService::handle(bytes,sizeof(bytes),out,sizeof(out))); Response response; assert(decode_response(out,sizeof(out),response)); return response;
}
static Response status(const Request &r) {
    Request query; query.operation=Operation::STATUS; query.transaction=r.transaction; memcpy(query.uid,r.uid,6);
    uint8_t bytes[REQUEST_SIZE]; assert(encode_request(r,bytes,sizeof(bytes))); query.value_length=32;
    crypto_blake2b_general(query.value,32,nullptr,0,bytes,sizeof(bytes)); crypto_wipe(bytes,sizeof(bytes)); return call(query);
}
static Response finish(const Request &r) { assert(call(r).result==Result::ACCEPTED); ManagementService::process(); ManagementService::update(); return status(r); }
static Request set_integer(const char *name,Type type,uint32_t value) {
    Request r=request(Operation::SET); strcpy(r.name,name); r.type=type;
    r.value_length=type==Type::UINT32?4:1; if(r.value_length==4) write_u32(r.value,value); else r.value[0]=uint8_t(value); return r;
}
static Request set_string(const char *name,Type type,const std::string &value) {
    Request r=request(Operation::SET); strcpy(r.name,name); r.type=type; r.value_length=value.size(); memcpy(r.value,value.data(),value.size()); return r;
}
static void factory_provisioning() {
    const auto old_activation=activation;
    auto blank=[&]() {
        activation=platform_activation::State{};
        memcpy(activation.snapshot.uid,old_activation.snapshot.uid,6);
        activation.snapshot.boot_id=old_activation.snapshot.boot_id+100;
        activation.provision_available=true; provision_uncertain=false;
        g.lock_level=0; fault_clear(); ManagementService::init();
    };
    auto factory_request=[]() {
        Request r=request(Operation::PROVISION); r.value_length=21; r.value[0]=2;
        memcpy(r.value+1,"AMOV0123456789012345",20); return r;
    };
    blank();
    const char *retained_key="PUBLIC_KEYV1:z+v8TvmElShJLVpTfkNH8Ei+DKXUuhydEGhFryeiVOg=";
    const char *retained_ssid="factory-retained";
    assert(finish(set_string("PUBLIC_KEY4",Type::CHAR64,retained_key)).result==Result::SAVED);
    assert(finish(set_string("WIFI_SSID",Type::CHAR20,retained_ssid)).result==Result::SAVED);
    assert(finish(set_integer("CAN_NODE",Type::UINT8,61)).result==Result::SAVED);
    auto info=call(request(Operation::INFO));
    assert(info.default_length==1 && info.default_value[0]==(FACTORY_PROVISION_CAPABLE|FACTORY_PROVISION_AVAILABLE));
    auto r=factory_request(); auto bad=r; bad.uid[0]^=1; assert(call(bad).result==Result::CONFLICT);
    bad=r; bad.boot++; assert(call(bad).result==Result::CONFLICT);
    bad=r; bad.value[4]=' '; assert(call(bad).result==Result::INVALID);
    bad.value[4]=0; assert(call(bad).result==Result::INVALID);
    g.lock_level=1; assert(call(r).result==Result::LOCKED);
    assert(call(request(Operation::INFO)).default_value[0]==FACTORY_PROVISION_CAPABLE);
    g.lock_level=0;
    const auto params_before=database["storage"];
    auto parameters_preserved=[&]() {
        const auto &actual=database["storage"];
        assert(actual.size()==params_before.size());
        for(const auto &item:params_before) {
            assert(actual.at(item.first).type==item.second.type);
            assert(actual.at(item.first).bytes==item.second.bytes);
        }
        assert(!strcmp(g.public_keys[3].b64_key,retained_key));
        assert(!strcmp(g.wifi_ssid,retained_ssid) && g.can_node==61);
    };
    const unsigned writes_before=provision_calls;
    const auto result=finish(r);
    assert(result.result==Result::SAVED && (result.flags&DURABLE) && !(result.flags&REBOOT_REQUIRED));
    assert(provision_calls==writes_before+1 && activation.snapshot.state==rid_activation::State::UNACTIVATED);
    assert(activation.snapshot.identity.revision==1 && activation.generation==1);
    assert(!strcmp(g.cn_uas_id,"AMOV0123456789012345") && g.cn_class==2 && g.cn_uin[0]==0);
    assert(call(request(Operation::INFO)).default_value[0]==FACTORY_PROVISION_CAPABLE);
    assert(call(r).result==Result::SAVED && provision_calls==writes_before+1);
    bad=r; bad.value[0]=3; assert(call(bad).result==Result::CONFLICT && status(bad).result==Result::CONFLICT);
    bad=factory_request(); assert(call(bad).result==Result::CONFLICT);
    bad.value[1]='X'; assert(call(bad).result==Result::CONFLICT);
    parameters_preserved();
    boot(); assert(status(r).result==Result::SAVED && call(factory_request()).result==Result::CONFLICT);
    parameters_preserved();

    blank(); r=factory_request(); assert(call(r).result==Result::ACCEPTED);
    g.lock_level=1; ManagementService::process(); ManagementService::update();
    assert(status(r).result==Result::LOCKED && !activation.provisioned);

    blank(); r=factory_request(); failure_namespace="rid_management"; fail_commit_at=1; commit_then_fail=true;
    const unsigned calls_before=provision_calls;
    assert(finish(r).result==Result::UNKNOWN && provision_calls==calls_before);
    boot(); assert(status(r).result==Result::UNKNOWN && !activation.provisioned && provision_calls==calls_before);

    blank(); r=factory_request(); provision_uncertain=true;
    assert(finish(r).result==Result::UNKNOWN && activation.provisioned);
    activation.snapshot.result=rid_activation::Result::STORAGE_ERROR; boot();
    assert(status(r).result==Result::UNKNOWN && !(status(r).flags&DURABLE));
    activation.snapshot.result=rid_activation::Result::OK; activation.generation=0; boot();
    assert(status(r).result==Result::UNKNOWN && !(status(r).flags&DURABLE));
    activation.generation=1;
    boot(); assert(status(r).result==Result::SAVED && (status(r).flags&DURABLE));

    blank(); r=factory_request(); failure_namespace="rid_management"; fail_commit_at=2;
    assert(finish(r).result==Result::UNKNOWN && activation.provisioned);
    activation.snapshot.identity.aircraft_class=3; boot();
    assert(status(r).result==Result::UNKNOWN); // Same SN alone cannot prove this write succeeded.
    activation.snapshot.identity.aircraft_class=2; boot(); assert(status(r).result==Result::SAVED);
    activation.snapshot.state=rid_activation::State::ACTIVE; boot();
    assert(status(r).result==Result::SAVED); // Later activation does not invalidate the original receipt.
}
int main(int argc,char **argv) {
    if(argc==3 && !strcmp(argv[1],"--digest-vector")) {
        std::ifstream input(argv[2],std::ios::binary); std::vector<uint8_t> vector((std::istreambuf_iterator<char>(input)),{});
        assert(vector.size()==REQUEST_SIZE+32); uint8_t digest[32];
        crypto_blake2b_general(digest,32,nullptr,0,vector.data(),REQUEST_SIZE);
        assert(!memcmp(digest,vector.data()+REQUEST_SIZE,32)); return 0;
    }
    uint8_t text_boundary[65]; memset(text_boundary,'A',sizeof(text_boundary));
    assert(ParameterText::valid(text_boundary,20,20) && !ParameterText::valid(text_boundary,21,20));
    assert(ParameterText::valid(text_boundary,64,64) && !ParameterText::valid(text_boundary,65,64));
    text_boundary[10]=0;
    assert(!ParameterText::valid(text_boundary,20,20) && !ParameterText::valid(text_boundary,64,64));
    assert(ParameterText::valid(nullptr,0,20) && !ParameterText::valid(nullptr,1,20));
    esp_read_mac(activation.snapshot.uid,0); activation.snapshot.boot_id=10; activation.snapshot.identity.revision=7;
    memcpy(activation.snapshot.identity.uid,activation.snapshot.uid,6); strcpy(activation.snapshot.identity.product,"AMOV0123456789012345");
    activation.snapshot.identity.aircraft_class=2; rid_activation::identity_hash(activation.snapshot.identity);
    activation.snapshot.state=rid_activation::State::ACTIVE; activation.source=platform_activation::Source::IMPORTED; activation.generation=1; activation.provisioned=true;
    boot();
    auto info=call(request(Operation::INFO)); assert(info.total==47 && info.revision==1 && (info.flags&LEGACY_IDENTITY));
    unsigned writable=0,masked=0,unused=0; std::map<std::string,Response> metadata;
    for(unsigned i=0;i<47;++i) { auto r=request(Operation::METADATA); r.index=i; auto value=call(r); assert(value.result==Result::OK); metadata[value.name]=value; writable+=(value.flags&WRITABLE)!=0; masked+=(value.flags&MASKED)!=0; unused+=(value.flags&UNUSED_IN_PROFILE)!=0; }
    assert(metadata.size()==47 && writable==31 && masked==1 && unused==16);
    if(argc==2 && !strcmp(argv[1],"--dump-json")) {
        std::cout<<"[\n"; unsigned count=0;
        for(unsigned i=0;i<47;++i) {
            const auto r=request(Operation::METADATA); auto indexed=r; indexed.index=i; const auto item=call(indexed);
            if(count++) std::cout<<",\n";
            std::cout<<"  {\"name\":\""<<item.name<<"\",\"type\":"<<unsigned(item.type)
                <<",\"flags\":"<<item.flags<<",\"minimum\":"<<item.minimum<<",\"maximum\":"<<item.maximum
                <<",\"min_length\":"<<unsigned(item.min_length)<<",\"max_length\":"<<unsigned(item.max_length)
                <<",\"default_hex\":\"";
            constexpr char hex[]="0123456789ABCDEF";
            for(unsigned j=0;j<item.default_length;++j) std::cout<<hex[item.default_value[j]>>4]<<hex[item.default_value[j]&15];
            std::cout<<"\"}";
        }
        std::cout<<"\n]\n"; return 0;
    }
    assert(metadata.count("DONE_INIT")==0 && metadata.count("CAN_TERMINATE")==0);
    assert((metadata["CAN_NODE"].flags&REBOOT_REQUIRED) && !(metadata["CN_CAN_NODE"].flags&REBOOT_REQUIRED));
    for(const char *name:{"RID_PROFILE","CN_UAS_ID","CN_UIN","CN_CLASS","RID_ACT_STATE","RID_UID"}) {
        auto r=set_integer(name,Type::UINT8,1); assert(call(r).result==Result::READ_ONLY);
    }
    for(const auto &item:metadata) {
        if(item.second.flags&WRITABLE) continue;
        auto r=set_integer(item.first.c_str(),Type::UINT8,1); assert(call(r).result==Result::READ_ONLY);
    }
    auto embedded=set_string("WIFI_SSID",Type::CHAR20,std::string("ab\0cd",5)); assert(call(embedded).result==Result::INVALID);
    auto uint8=set_integer("CAN_NODE",Type::UINT8,42); auto saved=finish(uint8);
    assert(saved.result==Result::SAVED && (saved.flags&DURABLE) && g.can_node==42);
    const auto writes=write_calls; assert(call(uint8).result==Result::SAVED && write_calls==writes);
    auto collision=uint8; collision.value[0]=43; assert(call(collision).result==Result::CONFLICT);
    auto integer=set_integer("BAUDRATE",Type::UINT32,921600); assert(finish(integer).result==Result::SAVED && g.baudrate==921600);
    integer=set_integer("BAUDRATE",Type::UINT32,921601); assert(call(integer).result==Result::INVALID);
    auto real=request(Operation::SET); strcpy(real.name,"WIFI_POWER"); real.type=Type::FLOAT; real.value_length=4;
    write_float(real.value,std::numeric_limits<float>::quiet_NaN()); assert(call(real).result==Result::INVALID);
    write_float(real.value,18.5f); assert(finish(real).result==Result::SAVED && g.wifi_power==18.5f);
    auto public_key=set_string("PUBLIC_KEY5",Type::CHAR64,std::string(64,'K')); assert(finish(public_key).result==Result::SAVED && strlen(g.public_keys[4].b64_key)==64);
    auto password=set_string("WIFI_PASSWORD",Type::CHAR20,"private-pass-123"); auto password_response=finish(password);
    assert(password_response.result==Result::SAVED && (password_response.flags&MASKED) && password_response.value_length==8);
    assert(!memcmp(password_response.value,"********",8) && !strcmp(g.wifi_password,"private-pass-123"));
    for(const auto &item:database["rid_management"]) {
        const auto &bytes=item.second.bytes; const std::string contents(bytes.begin(),bytes.end()); assert(contents.find("private-pass-123")==std::string::npos);
    }
    auto other_password=password; memcpy(other_password.value,"another-pass-456",16);
    // Client B loses its conflicting SET response and must not mistake A's
    // masked SAVED receipt for successful storage of B's different password.
    assert(call(other_password).result==Result::CONFLICT);
    assert(status(other_password).result==Result::CONFLICT && status(password).result==Result::SAVED);
    boot();
    assert(status(other_password).result==Result::CONFLICT && status(password).result==Result::SAVED);
    assert(!strcmp(g.wifi_password,"private-pass-123"));
    auto too_short=set_string("WIFI_PASSWORD",Type::CHAR20,"short"); assert(call(too_short).result==Result::INVALID);
    auto bad_wifi=set_string("CN_WIFI_ID",Type::CHAR20,"FA0BBC0D"); assert(call(bad_wifi).result==Result::INVALID);
    auto lock=set_integer("LOCK_LEVEL",Type::INT8,1); assert(finish(lock).result==Result::SAVED && g.lock_level==1);
    auto unlock=set_integer("LOCK_LEVEL",Type::INT8,0); assert(call(unlock).result==Result::LOCKED);
    auto activate=request(Operation::ACTIVATE); activate.revision=activation.generation; activate.value_length=21; activate.value[0]=0; memcpy(activate.value+1,activation.snapshot.identity.product,20);
    assert(finish(activate).result==Result::SAVED && activation.snapshot.state==rid_activation::State::UNACTIVATED);
    auto stale=activate; stale.transaction+=100000; assert(call(stale).result==Result::CONFLICT);
    assert(activation.snapshot.identity.revision==7 && activation.generation==2);
    g.lock_level=0; // Simulate the already-supported SecureCommand maintenance authority.
    auto pending=set_integer("CAN_NODE",Type::UINT8,50); assert(call(pending).result==Result::ACCEPTED && ManagementService::busy());
    assert(call(set_integer("CAN_NODE",Type::UINT8,51)).result==Result::BUSY);
    ManagementService::process(); assert(g.can_node==42); ManagementService::update(); assert(g.can_node==50);
    auto failed=set_integer("CAN_NODE",Type::UINT8,60); failure_namespace="storage"; fail_commit_at=1; commit_then_fail=true;
    assert(finish(failed).result==Result::UNKNOWN && g.can_node==50); boot(); assert(g.can_node==60);
    g.lock_level=0;
    auto lost=set_integer("CAN_NODE",Type::UINT8,70); failure_namespace="rid_management"; fail_commit_at=2;
    assert(finish(lost).result==Result::UNKNOWN && g.can_node==70); boot(); assert(status(lost).result==Result::SAVED && g.can_node==70);
    g.lock_level=0;
    auto unreadable=set_integer("CAN_NODE",Type::UINT8,80); failure_namespace="storage"; fail_read_at=2;
    assert(finish(unreadable).result==Result::UNKNOWN && g.can_node==70); boot(); assert(g.can_node==80);
    g.lock_level=0;
    auto reset=request(Operation::RESET); failure_namespace="rid_management"; fail_commit_at=3;
    const unsigned prior_erases=erases; assert(finish(reset).result==Result::UNKNOWN && erases==prior_erases+1 && ManagementService::busy());
    boot(); assert(status(reset).result==Result::SAVED && erases==prior_erases+1 && g.can_node==0);
    assert(activation.snapshot.identity.revision==7 && activation.snapshot.state==rid_activation::State::UNACTIVATED);
    boot(); assert(erases==prior_erases+1); // A terminal reset is never replayed.
    g.lock_level=1; // Existing RestartNode remains available even when parameters are locked.
    auto reset_locked=request(Operation::RESET); assert(call(reset_locked).result==Result::LOCKED);
    g.lock_level=0;
    auto uncertain_reset=request(Operation::RESET); failure_namespace="rid_management"; fail_commit_at=1; commit_then_fail=true;
    const unsigned reset_before=erases;
    assert(finish(uncertain_reset).result==Result::UNKNOWN && erases==reset_before && ManagementService::busy());
    boot(); assert(status(uncertain_reset).result==Result::SAVED && erases==reset_before+1);
    boot(); assert(erases==reset_before+1); // An accepted-but-unconfirmed reset is resumed once.
    g.lock_level=1;
    auto restart=request(Operation::REBOOT); assert(finish(restart).result==Result::SAVED); clock_ms+=800; ManagementService::update(); assert(restarts==1);
    factory_provisioning();
    std::cout<<"RID management: real metadata, locks, persistence, activation, reset and factory provisioning recovery passed\n";
}
