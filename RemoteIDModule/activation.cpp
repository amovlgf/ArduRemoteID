#include "activation.h"
#include "monocypher.h"
#include <string.h>

namespace rid_activation {
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
