#include "management_parameters.h"
#include "activation_service.h"
#include "CNData.h"
#include "monocypher.h"
#include "romfs.h"
#include "parameter_text.h"
#include <nvs.h>
#include <cmath>
#include <cstring>

namespace ManagedParameters {
using namespace amov_device;
namespace {
struct WipeOnExit { void *bytes; size_t size; ~WipeOnExit() { crypto_wipe(bytes,size); } };
const char *public_defaults[3] {};
bool defaults_loaded=false;
const char *const identity_names[]={"RID_ID_SCHEMA","RID_ID_REV","RID_BOOT_ID","RID_ACT_STATE","RID_UAS_ID","RID_REG_ID","RID_UIN","RID_CLASS","RID_UID","RID_ID_HASH","RID_PENDING","RID_ACT_ERR"};
bool readonly(const char *name) { return rid_activation::protected_parameter(name) || !strcmp(name,"RID_PROFILE"); }
bool reboot(const char *name) {
    const char *const names[]={"CAN_NODE","BAUDRATE","WIFI_SSID","WIFI_PASSWORD","WIFI_CHANNEL","WIFI_POWER"};
    for(const char *entry:names) { if(!strcmp(entry,name)) return true; }
    return false;
}
bool unused(const char *name) {
    const char *const names[]={"UAS_TYPE","UAS_ID_TYPE","UAS_ID","UAS_TYPE_2","UAS_ID_TYPE_2","UAS_ID_2","WIFI_NAN_RATE","WIFI_BCN_RATE","BT4_RATE","BT4_POWER","BT5_RATE","BT5_POWER","BCAST_POWERUP","MAVLINK_SYSID","OPTIONS","WEBSERVER_EN"};
    for(const char *entry:names) { if(!strcmp(entry,name)) return true; }
    return false;
}
uint16_t ordinary_count() {
    uint16_t count=0;
    for(const auto *p=Parameters::params;p->ptype!=Parameters::ParamType::NONE;++p) if(!(p->flags&PARAM_FLAG_HIDDEN)) ++count;
    return count;
}
void read_ram(const Parameters::Param &p,uint8_t *value,uint8_t &length) {
    switch(p.ptype) {
    case Parameters::ParamType::UINT8: value[0]=p.get_uint8(); length=1; break;
    case Parameters::ParamType::INT8: value[0]=uint8_t(p.get_int8()); length=1; break;
    case Parameters::ParamType::UINT32: write_u32(value,p.get_uint32()); length=4; break;
    case Parameters::ParamType::FLOAT: write_float(value,p.get_float()); length=4; break;
    case Parameters::ParamType::CHAR20: {
        const char *text=(p.flags&PARAM_FLAG_PASSWORD)?"********":p.get_char20(); length=strlen(text); memcpy(value,text,length); break;
    }
    case Parameters::ParamType::CHAR64: length=strlen(p.get_char64()); memcpy(value,p.get_char64(),length); break;
    default: length=0; break;
    }
}
bool stored(const Parameters::Param &p,uint8_t *value,uint8_t &length) {
    nvs_handle handle=0; if(nvs_open("storage",NVS_READONLY,&handle)!=ESP_OK) return false;
    esp_err_t result=ESP_FAIL;
    switch(p.ptype) {
    case Parameters::ParamType::UINT8: length=1; result=nvs_get_u8(handle,p.name,value); break;
    case Parameters::ParamType::INT8: length=1; result=nvs_get_i8(handle,p.name,reinterpret_cast<int8_t *>(value)); break;
    case Parameters::ParamType::FLOAT:
    case Parameters::ParamType::UINT32: { uint32_t integer=0; result=nvs_get_u32(handle,p.name,&integer); write_u32(value,integer); length=4; break; }
    case Parameters::ParamType::CHAR20:
    case Parameters::ParamType::CHAR64: { char text[65] {}; WipeOnExit wipe{text,sizeof(text)}; size_t size=sizeof(text); result=nvs_get_str(handle,p.name,text,&size); if(result==ESP_OK && size>0 && size<=sizeof(text)) { length=size-1; memcpy(value,text,length); } else result=ESP_FAIL; break; }
    default: break;
    }
    nvs_close(handle); return result==ESP_OK;
}
}
void initialize() {
    if(defaults_loaded) return;
    // ROMFS returns an allocated decompressed string. Cache each immutable key
    // for the device lifetime rather than decompressing/leaking on every GET.
    public_defaults[0]=ROMFS::find_string("public_keys/ArduPilot_public_key1.dat");
    public_defaults[1]=ROMFS::find_string("public_keys/ArduPilot_public_key2.dat");
    public_defaults[2]=ROMFS::find_string("public_keys/ArduPilot_public_key3.dat");
    defaults_loaded=true;
}
uint16_t count() { return ordinary_count()+sizeof(identity_names)/sizeof(identity_names[0]); }
bool describe(uint16_t index,Response &r) {
    const uint16_t ordinary=ordinary_count(); r.index=index; r.total=count();
    if(index>=r.total) return false;
    if(index>=ordinary) {
        const char *name=identity_names[index-ordinary]; rid_activation::ParamValue value;
        if(!rid_activation::get_parameter(ActivationService::snapshot(),name,value)) return false;
        strcpy(r.name,name);
        if(value.is_string) {
            r.value_length=strlen(value.string); r.type=(!strcmp(name,"RID_REG_ID") || !strcmp(name,"RID_ID_HASH"))?Type::CHAR64:Type::CHAR20;
            r.max_length=r.type==Type::CHAR64?64:20; memcpy(r.value,value.string,r.value_length);
        } else { r.type=Type::UINT32; r.value_length=4; write_u32(r.value,value.integer); }
        return true;
    }
    const Parameters::Param *p=nullptr; uint16_t visible=0;
    for(const auto *entry=Parameters::params;entry->ptype!=Parameters::ParamType::NONE;++entry) {
        if(entry->flags&PARAM_FLAG_HIDDEN) continue;
        if(visible++==index) { p=entry; break; }
    }
    if(!p) return false;
    strcpy(r.name,p->name); r.type=Type(p->ptype); r.minimum=p->min_value; r.maximum=p->max_value;
    if(!readonly(p->name) && g.lock_level<=0) r.flags|=WRITABLE;
    if(reboot(p->name)) r.flags|=REBOOT_REQUIRED;
    if(unused(p->name)) r.flags|=UNUSED_IN_PROFILE;
    if(!strcmp(p->name,"TO_DEFAULTS")) r.flags|=ACTION;
    if(p->flags&PARAM_FLAG_PASSWORD) r.flags|=MASKED;
    if(p->ptype==Parameters::ParamType::CHAR20 || p->ptype==Parameters::ParamType::CHAR64) {
        r.max_length=p->ptype==Parameters::ParamType::CHAR20?20:64; r.min_length=p->min_len;
        if(!strcmp(p->name,"CN_WIFI_ID")) r.max_length=8;
        const char *def=nullptr;
        if(!strcmp(p->name,"UAS_ID") || !strcmp(p->name,"UAS_ID_2")) def="ABCD123456789";
        if(!strcmp(p->name,"WIFI_SSID")) def="B950_RID";
        if(!strcmp(p->name,"CN_WIFI_ID")) def="";
        if(!strcmp(p->name,"PUBLIC_KEY1")) def=public_defaults[0];
        if(!strcmp(p->name,"PUBLIC_KEY2")) def=public_defaults[1];
        if(!strcmp(p->name,"PUBLIC_KEY3")) def=public_defaults[2];
        if(!strcmp(p->name,"PUBLIC_KEY4") || !strcmp(p->name,"PUBLIC_KEY5")) def="";
        if(def && strlen(def)<=r.max_length) { r.flags|=DEFAULT_AVAILABLE; r.default_length=strlen(def); memcpy(r.default_value,def,r.default_length); }
    } else if(!readonly(p->name)) {
        r.flags|=DEFAULT_AVAILABLE;
        if(p->ptype==Parameters::ParamType::FLOAT) { r.default_length=4; write_float(r.default_value,p->default_value); }
        else if(p->ptype==Parameters::ParamType::UINT32) { r.default_length=4; write_u32(r.default_value,uint32_t(p->default_value)); }
        else { r.default_length=1; r.default_value[0]=uint8_t(int(p->default_value)); }
    }
    read_ram(*p,r.value,r.value_length); return true;
}
bool describe(const char *name,Response &r) {
    for(uint16_t i=0;i<count();++i) { Response item; if(describe(i,item) && !strcmp(item.name,name)) {
        const auto op=r.operation; const auto transaction=r.transaction; const auto boot=r.boot; uint8_t uid[6]; memcpy(uid,r.uid,6);
        r=item; r.operation=op; r.transaction=transaction; r.boot=boot; memcpy(r.uid,uid,6); return true;
    } } return false;
}
Result validate(const Request &request) {
    const auto *p=Parameters::find(request.name);
    if(!p || (p->flags&PARAM_FLAG_HIDDEN) || p->ptype==Parameters::ParamType::NONE) {
        rid_activation::ParamValue ignored; return rid_activation::get_parameter(ActivationService::snapshot(),request.name,ignored)?Result::READ_ONLY:Result::NOT_FOUND;
    }
    if(readonly(p->name)) return Result::READ_ONLY;
    if(g.lock_level>0) return Result::LOCKED;
    if(Type(p->ptype)!=request.type) return Result::INVALID;
    if(!strcmp(p->name,"TO_DEFAULTS")) return Result::INVALID; // RESET is an explicit action.
    double numeric=0;
    switch(request.type) {
    case Type::UINT8: if(request.value_length!=1) return Result::INVALID; numeric=request.value[0]; break;
    case Type::INT8: if(request.value_length!=1) return Result::INVALID; numeric=int8_t(request.value[0]); break;
    case Type::UINT32: if(request.value_length!=4) return Result::INVALID; numeric=read_u32(request.value); break;
    case Type::FLOAT: if(request.value_length!=4) return Result::INVALID; numeric=read_float(request.value); if(!std::isfinite(numeric)) return Result::INVALID; break;
    case Type::CHAR20:
    case Type::CHAR64: {
        const uint8_t maximum=request.type==Type::CHAR20?20:64;
        if(!ParameterText::valid(request.value,request.value_length,maximum) || request.value_length<p->min_len) return Result::INVALID;
        if(!strcmp(p->name,"CN_WIFI_ID")) {
            char text[21] {}; memcpy(text,request.value,request.value_length); uint8_t id[4];
            if(request.value_length && (request.value_length!=8 || !CNData::parse_wifi_id(text,id))) return Result::INVALID;
        }
        return Result::OK;
    }
    default: return Result::INVALID;
    }
    return numeric<p->min_value || numeric>p->max_value?Result::INVALID:Result::OK;
}
Result persist(const Request &request) {
    const auto check=validate(request); if(check!=Result::OK) return check;
    const auto *p=Parameters::find(request.name); uint8_t previous[64] {}; WipeOnExit wipe{previous,sizeof(previous)}; uint8_t length=0;
    if(stored(*p,previous,length) && length==request.value_length && !memcmp(previous,request.value,length)) return Result::SAVED;
    nvs_handle handle=0; if(nvs_open("storage",NVS_READWRITE,&handle)!=ESP_OK) return Result::STORAGE_ERROR;
    esp_err_t result=ESP_FAIL;
    switch(request.type) {
    case Type::UINT8: result=nvs_set_u8(handle,p->name,request.value[0]); break;
    case Type::INT8: result=nvs_set_i8(handle,p->name,int8_t(request.value[0])); break;
    case Type::UINT32:
    case Type::FLOAT: result=nvs_set_u32(handle,p->name,read_u32(request.value)); break;
    case Type::CHAR20:
    case Type::CHAR64: { char text[65] {}; WipeOnExit wipe{text,sizeof(text)}; memcpy(text,request.value,request.value_length); result=nvs_set_str(handle,p->name,text); break; }
    default: break;
    }
    if(result==ESP_OK) result=nvs_commit(handle);
    nvs_close(handle);
    if(result!=ESP_OK || !stored(*p,previous,length) || length!=request.value_length || memcmp(previous,request.value,length)) return Result::UNKNOWN;
    return Result::SAVED;
}
void apply(const Request &request) {
    const auto *p=Parameters::find(request.name); if(!p) return;
    switch(request.type) {
    case Type::UINT8: *static_cast<uint8_t *>(const_cast<void *>(p->ptr))=request.value[0]; break;
    case Type::INT8: *static_cast<int8_t *>(const_cast<void *>(p->ptr))=int8_t(request.value[0]); break;
    case Type::UINT32: *static_cast<uint32_t *>(const_cast<void *>(p->ptr))=read_u32(request.value); break;
    case Type::FLOAT: *static_cast<float *>(const_cast<void *>(p->ptr))=read_float(request.value); break;
    case Type::CHAR20:
    case Type::CHAR64: { char *text=static_cast<char *>(const_cast<void *>(p->ptr)); const size_t size=request.type==Type::CHAR20?21:65; memset(text,0,size); memcpy(text,request.value,request.value_length); break; }
    default: break;
    }
}
bool reset() {
    nvs_handle handle=0; if(nvs_open("storage",NVS_READWRITE,&handle)!=ESP_OK) return false;
    bool ok=nvs_erase_all(handle)==ESP_OK && nvs_commit(handle)==ESP_OK; nvs_close(handle);
    if(!ok) return false;
    if(nvs_open("storage",NVS_READONLY,&handle)!=ESP_OK) return false;
    for(const auto *p=Parameters::params;p->ptype!=Parameters::ParamType::NONE;++p) {
        uint8_t value=0; if(nvs_get_u8(handle,p->name,&value)!=ESP_ERR_NVS_NOT_FOUND) { nvs_close(handle); return false; }
    }
    nvs_close(handle); return true;
}
void value_digest(Type type,const uint8_t *value,uint8_t size,uint8_t out[32]) {
    uint8_t bytes[66] {}; bytes[0]=uint8_t(type); bytes[1]=size; memcpy(bytes+2,value,size);
    crypto_blake2b_general(out,32,nullptr,0,bytes,size+2); crypto_wipe(bytes,sizeof(bytes));
}
bool stored_digest(const char *name,uint8_t out[32]) {
    const auto *p=Parameters::find(name); if(!p) return false; uint8_t bytes[64] {}; WipeOnExit wipe{bytes,sizeof(bytes)}; uint8_t size=0;
    if(!stored(*p,bytes,size)) return false;
    value_digest(Type(p->ptype),bytes,size,out); return true;
}
}
