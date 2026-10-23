#include "management_service.h"
#include "management_parameters.h"
#include "activation_service.h"
#include "monocypher.h"
#include <Arduino.h>
#include <esp_system.h>
#include <nvs.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <cstring>

namespace ManagementService {
using namespace amov_device;
namespace {
constexpr unsigned LEDGER_COUNT=8;
constexpr size_t RECORD_SIZE=264;
struct Entry { bool valid=false; uint32_t sequence=0; uint8_t request_hash[32] {}; uint8_t value_hash[32] {}; Response response; };
struct Completion { Entry entry; Request request; unsigned slot=0; bool apply=false; bool identity_changed=false; bool reboot=false; };
Entry ledger[LEDGER_COUNT];
QueueHandle_t requests=nullptr;
portMUX_TYPE mux=portMUX_INITIALIZER_UNLOCKED;
bool outstanding=false;
bool completed=false;
bool ready=false;
bool recovery_blocked=false;
Request active_request;
uint8_t active_hash[32] {};
Response active_response;
Completion completion;
uint32_t sequence=0;
uint32_t reboot_at=0;
nvs_handle storage=0;
void stamp(Response &r) {
    const auto snapshot=ActivationService::snapshot(); const auto platform=ActivationService::platform_state();
    r.boot=snapshot.boot_id; memcpy(r.uid,snapshot.uid,6); r.revision=platform.generation;
    r.activation_state=uint8_t(snapshot.state); r.total=ManagedParameters::count();
    r.flags&=~LEGACY_IDENTITY; if(platform.source==platform_activation::Source::IMPORTED) r.flags|=LEGACY_IDENTITY;
}
Response base(const Request &request) { Response r; r.operation=request.operation; r.transaction=request.transaction; stamp(r); return r; }
void request_digest(const Request &request,uint8_t out[32]) {
    uint8_t bytes[REQUEST_SIZE] {}; encode_request(request,bytes,sizeof(bytes)); crypto_blake2b_general(out,32,nullptr,0,bytes,sizeof(bytes)); crypto_wipe(bytes,sizeof(bytes));
}
bool save(unsigned slot,const Entry &entry) {
    uint8_t bytes[RECORD_SIZE] {}; write_u32(bytes,entry.sequence); memcpy(bytes+4,entry.request_hash,32); memcpy(bytes+36,entry.value_hash,32);
    if(!encode_response(entry.response,bytes+68,RESPONSE_SIZE)) return false;
    write_u32(bytes+260,platform_activation::crc32(bytes,260));
    char key[12]; snprintf(key,sizeof(key),"receipt%u",slot);
    if(nvs_set_blob(storage,key,bytes,sizeof(bytes))!=ESP_OK || nvs_commit(storage)!=ESP_OK) return false;
    uint8_t check[RECORD_SIZE]; size_t length=sizeof(check);
    return nvs_get_blob(storage,key,check,&length)==ESP_OK && length==sizeof(bytes) && !memcmp(bytes,check,length);
}
bool load(unsigned slot,Entry &entry) {
    char key[12]; snprintf(key,sizeof(key),"receipt%u",slot); uint8_t bytes[RECORD_SIZE]; size_t length=sizeof(bytes);
    if(nvs_get_blob(storage,key,bytes,&length)!=ESP_OK || length!=sizeof(bytes) ||
       read_u32(bytes+260)!=platform_activation::crc32(bytes,260) || !read_u32(bytes) ||
       !decode_response(bytes+68,RESPONSE_SIZE,entry.response)) return false;
    const auto snapshot=ActivationService::snapshot(); if(memcmp(entry.response.uid,snapshot.uid,6)) return false;
    entry.sequence=read_u32(bytes); memcpy(entry.request_hash,bytes+4,32); memcpy(entry.value_hash,bytes+36,32); entry.valid=true; return true;
}
bool plain(const Request &r) { return r.type==Type::NONE && r.value_length==0 && r.name[0]==0 && r.revision==0; }
Result activation_result(rid_activation::Result result) {
    switch(result) {
    case rid_activation::Result::OK: return Result::SAVED;
    case rid_activation::Result::WRONG_DEVICE: return Result::CONFLICT;
    case rid_activation::Result::VERSION_CONFLICT: return Result::CONFLICT;
    case rid_activation::Result::STORAGE_ERROR: return Result::UNKNOWN;
    case rid_activation::Result::NO_TRUST: return Result::LOCKED;
    default:return Result::INVALID;
    }
}
bool provision_matches(const Entry &entry) {
    const auto state=ActivationService::platform_state();
    const auto &id=state.snapshot.identity;
    if(!state.provisioned || state.snapshot.state==rid_activation::State::INVALID ||
       state.snapshot.result!=rid_activation::Result::OK || !state.generation || id.revision!=1 ||
       id.registration[0] || id.uin[0]) return false;
    uint8_t value[21], hash[32]; value[0]=id.aircraft_class; memcpy(value+1,id.product,20);
    ManagedParameters::value_digest(Type::NONE,value,sizeof(value),hash);
    return !memcmp(hash,entry.value_hash,32);
}
void block_recovery() { portENTER_CRITICAL(&mux); recovery_blocked=true; portEXIT_CRITICAL(&mux); }
enum class ResetMarker { MISSING, MATCH, DIFFERENT, ERROR };
ResetMarker reset_marker(const uint8_t hash[32]) {
    uint8_t bytes[36]; size_t size=sizeof(bytes); const auto result=nvs_get_blob(storage,"reset_done",bytes,&size);
    if(result==ESP_ERR_NVS_NOT_FOUND) return ResetMarker::MISSING;
    if(result!=ESP_OK || size!=sizeof(bytes) || read_u32(bytes+32)!=platform_activation::crc32(bytes,32)) return ResetMarker::ERROR;
    return !memcmp(bytes,hash,32)?ResetMarker::MATCH:ResetMarker::DIFFERENT;
}
bool mark_reset(const uint8_t hash[32]) {
    uint8_t bytes[36]; memcpy(bytes,hash,32); write_u32(bytes+32,platform_activation::crc32(bytes,32));
    return nvs_set_blob(storage,"reset_done",bytes,sizeof(bytes))==ESP_OK && nvs_commit(storage)==ESP_OK && reset_marker(hash)==ResetMarker::MATCH;
}
Result perform_reset(const uint8_t hash[32]) {
    const auto marker=reset_marker(hash);
    if(marker==ResetMarker::MATCH) return Result::SAVED;
    if(marker==ResetMarker::ERROR) { block_recovery(); return Result::UNKNOWN; }
    if(!ManagedParameters::reset() || !mark_reset(hash)) { block_recovery(); return Result::UNKNOWN; }
    return Result::SAVED;
}
void resolve_pending(Entry &entry) {
    if(entry.response.result!=Result::ACCEPTED &&
       !(entry.response.operation==Operation::PROVISION && entry.response.result==Result::UNKNOWN)) return;
    entry.response.result=Result::UNKNOWN;
    if(entry.response.operation==Operation::SET) {
        uint8_t actual[32];
        if(ManagedParameters::stored_digest(entry.response.name,actual) && !memcmp(actual,entry.value_hash,32)) entry.response.result=Result::SAVED;
    } else if(entry.response.operation==Operation::ACTIVATE) {
        const auto state=ActivationService::platform_state();
        const uint64_t transaction=(uint64_t(entry.response.boot)<<32)|entry.response.transaction;
        if(state.transaction==transaction && uint8_t(state.snapshot.state)==entry.response.activation_state) entry.response.result=Result::SAVED;
    } else if(entry.response.operation==Operation::PROVISION) {
        // Recovery only observes the record loaded on boot; never replay an
        // uncertain identity write or accept a different class with the same SN.
        if(provision_matches(entry)) entry.response.result=Result::SAVED;
    } else if(entry.response.operation==Operation::RESET) {
        const bool previously_done=reset_marker(entry.request_hash)==ResetMarker::MATCH;
        entry.response.result=perform_reset(entry.request_hash);
        if(entry.response.result==Result::SAVED && !previously_done) reboot_at=millis()+750;
    } else if(entry.response.operation==Operation::REBOOT) { entry.response.result=Result::SAVED; }
    if(entry.response.result==Result::SAVED) entry.response.flags|=DURABLE;
}
}
void init() {
    if(!ActivationService::required()) return;
    ManagedParameters::initialize();
    if(requests) vQueueDelete(requests);
    if(storage) nvs_close(storage);
    for(auto &entry:ledger) entry=Entry{};
    outstanding=false; completed=false; recovery_blocked=false;
    sequence=0; reboot_at=0; active_request=Request{}; completion=Completion{};
    requests=xQueueCreate(1,sizeof(uint8_t));
    ready=requests && nvs_open("rid_management",NVS_READWRITE,&storage)==ESP_OK;
    if(!ready) return;
    for(unsigned i=0;i<LEDGER_COUNT;++i) {
        Entry entry; if(!load(i,entry)) continue;
        sequence=entry.sequence>sequence?entry.sequence:sequence;
        const bool pending=entry.response.result==Result::ACCEPTED ||
            (entry.response.operation==Operation::PROVISION && entry.response.result==Result::UNKNOWN);
        resolve_pending(entry);
        if(pending && !save(i,entry)) { entry.response.result=Result::UNKNOWN; entry.response.flags&=~DURABLE; if(entry.response.operation==Operation::RESET) { recovery_blocked=true; reboot_at=0; } }
        ledger[i]=entry;
    }
}
bool busy() {
    portENTER_CRITICAL(&mux); const bool value=outstanding || reboot_at!=0 || recovery_blocked; portEXIT_CRITICAL(&mux); return value;
}
void update() {
    Completion done; bool have=false;
    portENTER_CRITICAL(&mux); if(completed) { done=completion; crypto_wipe(completion.request.value,sizeof(completion.request.value)); completion=Completion{}; have=true; completed=false; } portEXIT_CRITICAL(&mux);
    if(have) {
        if(done.apply) ManagedParameters::apply(done.request);
        if(done.identity_changed) {
            const auto snapshot=ActivationService::snapshot();
            memcpy(g.cn_uas_id,snapshot.identity.product,sizeof(snapshot.identity.product));
            memcpy(g.cn_uin,snapshot.identity.uin,sizeof(snapshot.identity.uin));
            g.cn_class=snapshot.identity.aircraft_class;
        }
        portENTER_CRITICAL(&mux);
        ledger[done.slot]=done.entry; outstanding=false; active_request=Request{}; memset(active_hash,0,sizeof(active_hash));
        if(done.reboot) reboot_at=millis()+750;
        portEXIT_CRITICAL(&mux);
        crypto_wipe(done.request.value,sizeof(done.request.value));
    }
    if(reboot_at && int32_t(millis()-reboot_at)>=0) esp_restart();
}
bool handle(const uint8_t *bytes,size_t size,uint8_t *output,size_t capacity) {
    Request request; if(!decode_request(bytes,size,request)) return false;
    Response response=base(request); const auto snapshot=ActivationService::snapshot();
    if(!ready || !ActivationService::required()) response.result=Result::UNSUPPORTED;
    else {
        const bool discovery=request.operation==Operation::INFO && !memcmp(request.uid,"\0\0\0\0\0\0",6);
        if(!discovery && memcmp(request.uid,snapshot.uid,6)) response.result=Result::CONFLICT;
        else if(request.operation==Operation::STATUS) {
            if(request.boot && request.boot!=snapshot.boot_id) response.result=Result::CONFLICT;
            else {
                bool found=false;
                portENTER_CRITICAL(&mux);
                if(outstanding && request.transaction==active_request.transaction) {
                    response=active_response; found=true;
                    if(memcmp(request.value,active_hash,32)) response.result=Result::CONFLICT;
                }
                else for(const auto &entry:ledger) if(entry.valid && entry.response.transaction==request.transaction) {
                    response=entry.response; found=true;
                    if(memcmp(request.value,entry.request_hash,32)) response.result=Result::CONFLICT;
                    break;
                }
                portEXIT_CRITICAL(&mux);
                if(!found) response.result=Result::UNKNOWN;
            }
        } else if(request.operation==Operation::INFO) {
            if(!plain(request) || request.index) response.result=Result::INVALID;
            else {
                response.default_length=1; response.default_value[0]=FACTORY_PROVISION_CAPABLE;
                if(ActivationService::factory_provision_available()) response.default_value[0]|=FACTORY_PROVISION_AVAILABLE;
                if(snapshot.identity.revision) { response.type=Type::CHAR20; response.value_length=20; response.max_length=20; memcpy(response.value,snapshot.identity.product,20); }
            }
        } else if(request.operation==Operation::METADATA || request.operation==Operation::GET) {
            if(request.boot!=snapshot.boot_id) response.result=Result::CONFLICT;
            else if(request.operation==Operation::METADATA) {
                if(!plain(request)) response.result=Result::INVALID;
                else if(!ManagedParameters::describe(request.index,response)) response.result=Result::NOT_FOUND;
            } else {
                if(request.type!=Type::NONE || request.value_length || !request.name[0] || request.revision || request.index) response.result=Result::INVALID;
                else if(!ManagedParameters::describe(request.name,response)) response.result=Result::NOT_FOUND;
            }
        } else {
            uint8_t hash[32]; request_digest(request,hash); bool duplicate=false;
            portENTER_CRITICAL(&mux);
            if(outstanding && request.transaction==active_request.transaction) {
                response=active_response; if(memcmp(hash,active_hash,32)) response.result=Result::CONFLICT; duplicate=true;
            } else for(const auto &entry:ledger) if(entry.valid && entry.response.transaction==request.transaction) {
                response=entry.response; if(memcmp(hash,entry.request_hash,32)) response.result=Result::CONFLICT; duplicate=true; break;
            }
            portEXIT_CRITICAL(&mux);
            if(!duplicate) {
                if(request.boot!=snapshot.boot_id) response.result=Result::CONFLICT;
                else if(busy()) response.result=Result::BUSY;
                else {
                    Result valid=Result::OK;
                    if(request.operation==Operation::SET) {
                        valid=request.index || request.revision?Result::INVALID:ManagedParameters::validate(request);
                        if(valid==Result::OK) ManagedParameters::describe(request.name,response);
                    } else if(request.operation==Operation::ACTIVATE) {
                        if(request.type!=Type::NONE || request.name[0] || request.index || request.value_length!=21 || request.value[0]>1) valid=Result::INVALID;
                        else if(!snapshot.identity.revision || request.revision!=ActivationService::platform_state().generation || memcmp(request.value+1,snapshot.identity.product,20)) valid=Result::CONFLICT;
                    } else if(request.operation==Operation::PROVISION) {
                        if(g.lock_level!=0) valid=Result::LOCKED;
                        else if(!ActivationService::factory_provision_available()) valid=Result::CONFLICT;
                        else for(unsigned i=1;i<21;++i) if(request.value[i]<33 || request.value[i]>126) { valid=Result::INVALID; break; }
                    } else if(request.operation==Operation::RESET || request.operation==Operation::REBOOT) {
                        if(!plain(request) || request.index) valid=Result::INVALID;
                        else if(request.operation==Operation::RESET && g.lock_level>0) valid=Result::LOCKED;
                    } else valid=Result::UNSUPPORTED;
                    response.result=valid;
                    if(valid==Result::OK) {
                        response.result=Result::ACCEPTED;
                        portENTER_CRITICAL(&mux); outstanding=true; active_request=request; memcpy(active_hash,hash,32); active_response=response; portEXIT_CRITICAL(&mux);
                        const uint8_t token=1;
                        if(xQueueSend(requests,&token,0)!=pdTRUE) {
                            portENTER_CRITICAL(&mux); crypto_wipe(active_request.value,sizeof(active_request.value)); active_request=Request{}; memset(active_hash,0,sizeof(active_hash)); outstanding=false; portEXIT_CRITICAL(&mux); response.result=Result::BUSY;
                        }
                    }
                }
            }
        }
    }
    stamp(response); const bool encoded=encode_response(response,output,capacity); crypto_wipe(request.value,sizeof(request.value)); return encoded;
}
void process() {
    uint8_t token=0; if(!ready || xQueueReceive(requests,&token,0)!=pdTRUE) return;
    Request request; portENTER_CRITICAL(&mux); request=active_request; crypto_wipe(active_request.value,sizeof(active_request.value)); portEXIT_CRITICAL(&mux);
    Completion done; done.request=request; done.slot=sequence%LEDGER_COUNT;
    Entry &entry=done.entry; entry.valid=true; entry.sequence=++sequence; entry.response=base(request); entry.response.result=Result::ACCEPTED;
    request_digest(request,entry.request_hash);
    if(request.operation==Operation::SET) {
        ManagedParameters::describe(request.name,entry.response); entry.response.result=Result::ACCEPTED;
        ManagedParameters::value_digest(request.type,request.value,request.value_length,entry.value_hash);
        if(!(entry.response.flags&MASKED)) { memset(entry.response.value,0,sizeof(entry.response.value)); memcpy(entry.response.value,request.value,request.value_length); entry.response.value_length=request.value_length; }
    } else if(request.operation==Operation::ACTIVATE) {
        entry.response.type=Type::CHAR20; entry.response.max_length=20; entry.response.value_length=20; memcpy(entry.response.value,request.value+1,20);
        entry.response.activation_state=request.value[0];
    } else if(request.operation==Operation::PROVISION) {
        entry.response.type=Type::CHAR20; entry.response.max_length=20; entry.response.value_length=20;
        memcpy(entry.response.value,request.value+1,20); entry.response.flags|=ACTION;
        ManagedParameters::value_digest(Type::NONE,request.value,request.value_length,entry.value_hash);
    } else entry.response.flags|=ACTION|REBOOT_REQUIRED;
    // A failed commit/readback may still have persisted the accepted intent.
    // After reboot it can be recovered; never report a definite rejection.
    if(!save(done.slot,entry)) {
        entry.response.result=Result::UNKNOWN;
        if(request.operation==Operation::RESET) block_recovery();
    }
    else {
        if(request.operation==Operation::SET) { entry.response.result=ManagedParameters::persist(request); done.apply=entry.response.result==Result::SAVED; }
        else if(request.operation==Operation::ACTIVATE) {
            const uint64_t transaction=(uint64_t(request.boot)<<32)|request.transaction;
            entry.response.result=activation_result(ActivationService::synchronize(reinterpret_cast<const char *>(request.value+1),request.value[0]!=0,transaction));
        } else if(request.operation==Operation::PROVISION) {
            entry.response.result=activation_result(ActivationService::provision(reinterpret_cast<const char *>(request.value+1),request.value[0]));
            done.identity_changed=true;
        } else if(request.operation==Operation::RESET) { entry.response.result=perform_reset(entry.request_hash); done.reboot=entry.response.result==Result::SAVED; }
        else if(request.operation==Operation::REBOOT) { entry.response.result=Result::SAVED; done.reboot=true; }
        if(entry.response.result==Result::SAVED) entry.response.flags|=DURABLE;
        if(!save(done.slot,entry)) { entry.response.result=Result::UNKNOWN; entry.response.flags&=~DURABLE; done.reboot=false; if(request.operation==Operation::RESET) block_recovery(); }
    }
    if(!done.apply) crypto_wipe(done.request.value,sizeof(done.request.value));
    portENTER_CRITICAL(&mux); completion=done; completed=true; portEXIT_CRITICAL(&mux);
    crypto_wipe(request.value,sizeof(request.value)); crypto_wipe(done.request.value,sizeof(done.request.value));
}
}
