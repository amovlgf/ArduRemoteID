#include "platform_activation.h"
#include <string.h>

namespace platform_activation {
using rid_activation::Storage;
using rid_activation::Result;
namespace {
uint32_t get32(const uint8_t *p) { return uint32_t(p[0]) | uint32_t(p[1])<<8 | uint32_t(p[2])<<16 | uint32_t(p[3])<<24; }
uint64_t get64(const uint8_t *p) { return uint64_t(get32(p)) | uint64_t(get32(p+4))<<32; }
void put32(uint8_t *p,uint32_t n) { for(unsigned i=0;i<4;++i) p[i]=uint8_t(n>>(i*8)); }
void put64(uint8_t *p,uint64_t n) { put32(p,uint32_t(n)); put32(p+4,uint32_t(n>>32)); }
bool printable(const uint8_t *p,size_t size) { for(size_t i=0;i<size;++i) if(p[i]<33 || p[i]>126) return false; return true; }
bool zero(const uint8_t *p,size_t size) { for(size_t i=0;i<size;++i) if(p[i]) return false; return true; }
bool verified_write(Storage &storage,const char *key,const uint8_t *p,size_t size) {
    if(!storage.write(key,p,size)) return false;
    uint8_t check[FACTORY_SIZE] {}; size_t count=0;
    return storage.read(key,check,sizeof(check),count)==Storage::Read::OK && count==size && !memcmp(p,check,size);
}
const char *const slots[]={"state_a","state_b"};
void digest(const char hash[65],uint8_t out[32]) {
    for(unsigned i=0;i<32;++i) {
        const auto hex=[](char c)->uint8_t { return c>='0' && c<='9' ? uint8_t(c-'0') : uint8_t(c-'A'+10); };
        out[i]=uint8_t((hex(hash[2*i])<<4)|hex(hash[2*i+1]));
    }
}
void encode_state(const State &state,uint8_t out[STATE_SIZE]) {
    memset(out,0,STATE_SIZE); memcpy(out,"AMOVSTA1",8); out[8]=1;
    out[9]=uint8_t(state.snapshot.state); out[10]=uint8_t(state.source);
    put32(out+12,state.generation); put64(out+16,state.transaction);
    memcpy(out+24,state.snapshot.uid,6); digest(state.snapshot.identity.hash,out+32);
    put32(out+64,crc32(out,64));
}
bool decode_state(const uint8_t *p,size_t size,const State &base,State &out) {
    uint8_t hash[32]; digest(base.snapshot.identity.hash,hash);
    if(size!=STATE_SIZE || memcmp(p,"AMOVSTA1",8) || p[8]!=1 || p[9]>1 || p[10]>2 || p[11] ||
       !get32(p+12) || memcmp(p+24,base.snapshot.uid,6) || p[30] || p[31] ||
       memcmp(p+32,hash,32) || get32(p+64)!=crc32(p,64)) return false;
    out=base; out.snapshot.state=rid_activation::State(p[9]); out.source=Source(p[10]);
    out.generation=get32(p+12); out.transaction=get64(p+16); return true;
}
}
uint32_t crc32(const uint8_t *p,size_t size) {
    uint32_t crc=0xffffffffU;
    for(size_t i=0;i<size;++i) { crc^=p[i]; for(unsigned b=0;b<8;++b) crc=(crc>>1)^(0xedb88320U & (0U-(crc&1U))); }
    return ~crc;
}
void encode_factory(const rid_activation::Identity &id,Source source,uint8_t out[FACTORY_SIZE]) {
    memset(out,0,FACTORY_SIZE); memcpy(out,"AMOVFID1",8); out[8]=1; out[9]=id.aircraft_class;
    out[10]=uint8_t(strlen(id.registration)); out[11]=uint8_t(strlen(id.uin)); put32(out+12,id.revision);
    memcpy(out+16,id.uid,6); memcpy(out+22,id.product,20); memcpy(out+42,id.registration,out[10]);
    memcpy(out+106,id.uin,out[11]); out[114]=uint8_t(source); put32(out+116,crc32(out,116));
}
bool decode_factory(const uint8_t *p,size_t size,const uint8_t uid[6],rid_activation::Identity &id,Source &source) {
    id={}; source=Source::FACTORY;
    if(!p || size!=FACTORY_SIZE || memcmp(p,"AMOVFID1",8) || p[8]!=1 || p[9]>4 || p[10]>64 ||
       (p[11]!=0 && p[11]!=8) || !get32(p+12) || memcmp(p+16,uid,6) || zero(uid,6) ||
       !printable(p+22,20) || !printable(p+42,p[10]) || !zero(p+42+p[10],64-p[10]) ||
       !printable(p+106,p[11]) || !zero(p+106+p[11],8-p[11]) || p[114]>1 || p[115] ||
       get32(p+116)!=crc32(p,116) || (p[10] && (p[10]<8 || p[11]!=8 || memcmp(p+42+p[10]-8,p+106,8)))) return false;
    id.revision=get32(p+12); memcpy(id.uid,uid,6); id.aircraft_class=p[9];
    memcpy(id.product,p+22,20); memcpy(id.registration,p+42,p[10]); memcpy(id.uin,p+106,p[11]);
    rid_activation::identity_hash(id); source=Source(p[114]); return true;
}
Manager::Manager(const uint8_t uid[6],uint32_t boot) {
    memcpy(current.snapshot.uid,uid,6); current.snapshot.boot_id=boot?boot:1;
}
void Manager::boot(Storage &storage) {
    const auto previous=current.snapshot;
    current={}; memcpy(current.snapshot.uid,previous.uid,6); current.snapshot.boot_id=previous.boot_id;
    slot=-1;
    if(zero(current.snapshot.uid,6)) {
        current.snapshot.state=rid_activation::State::INVALID;
        current.snapshot.result=Result::INVALID_IDENTITY;
        return;
    }
    uint8_t bytes[FACTORY_SIZE] {}; size_t length=0;
    const auto factory_read=storage.read("factory",bytes,sizeof(bytes),length);
    if(factory_read==Storage::Read::MISSING) {
        // Missing identity is not a blank device if state records remain or
        // cannot be read. Never replace a lost or unreadable factory record.
        const char *const historical_keys[]={"state_a", "state_b", "ready"};
        for(const char *key : historical_keys) {
            length=0;
            if(storage.read(key,bytes,sizeof(bytes),length)!=Storage::Read::MISSING) {
                current.snapshot.state=rid_activation::State::INVALID;
                current.snapshot.result=Result::STORAGE_ERROR;
                return;
            }
        }
        current.provision_available=true;
        return;
    }
    else if(factory_read!=Storage::Read::OK) { current.snapshot.state=rid_activation::State::INVALID; current.snapshot.result=Result::STORAGE_ERROR; return; }
    Source initial_source;
    if(!decode_factory(bytes,length,current.snapshot.uid,current.snapshot.identity,initial_source)) {
        current.snapshot.state=rid_activation::State::INVALID; current.snapshot.result=Result::INVALID_IDENTITY; return;
    }
    current.provisioned=true;
    bool damaged=false;
    for(int i=0;i<2;++i) {
        length=0; const auto read=storage.read(slots[i],bytes,sizeof(bytes),length);
        if(read==Storage::Read::MISSING) continue;
        State candidate;
        if(read!=Storage::Read::OK || !decode_state(bytes,length,current,candidate)) { damaged=true; continue; }
        if(slot>=0 && candidate.generation==current.generation &&
           (candidate.transaction!=current.transaction || candidate.source!=current.source || candidate.snapshot.state!=current.snapshot.state)) {
            current.snapshot.state=rid_activation::State::INVALID; current.snapshot.result=Result::VERSION_CONFLICT; return;
        }
        if(slot<0 || candidate.generation>current.generation) { current=candidate; slot=i; }
    }
    length=0; uint8_t ready[4] {}; const auto marker=storage.read("ready",ready,sizeof(ready),length);
    const bool initialized=marker==Storage::Read::OK && length==4 && !memcmp(ready,"RDY1",4);
    if(slot<0) {
        if(damaged || marker!=Storage::Read::MISSING) { current.snapshot.state=rid_activation::State::INVALID; current.snapshot.result=Result::STORAGE_ERROR; return; }
        State initial=current; initial.source=initial_source; initial.generation=1;
        // A factory identity (including a previously imported one) identifies
        // the aircraft; only an explicit platform synchronization activates it.
        initial.snapshot.state=rid_activation::State::UNACTIVATED;
        if(!save(storage,initial)) { current.snapshot.state=rid_activation::State::INVALID; current.snapshot.result=Result::STORAGE_ERROR; return; }
    }
    if(!initialized && !verified_write(storage,"ready",reinterpret_cast<const uint8_t *>("RDY1"),4)) current.snapshot.result=Result::STORAGE_ERROR;
}
bool Manager::save(Storage &storage,const State &next) {
    uint8_t bytes[STATE_SIZE]; encode_state(next,bytes); const int target=slot==0?1:0;
    if(!verified_write(storage,slots[target],bytes,sizeof(bytes))) return false;
    current=next; slot=target; return true;
}
Result Manager::provision(Storage &storage,const char sn[20],uint8_t aircraft_class) {
    if(!sn || aircraft_class>4 || !printable(reinterpret_cast<const uint8_t *>(sn),20) ||
       zero(current.snapshot.uid,6)) return current.snapshot.result=Result::INVALID_IDENTITY;

    // Recheck persistent state at execution time, even when an earlier INFO
    // response advertised a blank device. boot never overwrites factory data.
    boot(storage);
    if(!current.provision_available) {
        if(current.snapshot.result==Result::OK) current.snapshot.result=Result::VERSION_CONFLICT;
        return current.snapshot.result;
    }
    rid_activation::Identity identity;
    identity.revision=1;
    identity.aircraft_class=aircraft_class;
    memcpy(identity.uid,current.snapshot.uid,6);
    memcpy(identity.product,sn,20);
    uint8_t bytes[FACTORY_SIZE];
    encode_factory(identity,Source::FACTORY,bytes);
    const bool written=verified_write(storage,"factory",bytes,sizeof(bytes));
    // A failed commit/readback may still have persisted the identity. Publish
    // what storage now contains and refuse any overwrite on a later attempt.
    boot(storage);
    const bool initialized=current.snapshot.result==Result::OK;
    if(!initialized) boot(storage);
    if(!written || !initialized) return current.snapshot.result=Result::STORAGE_ERROR;
    return Result::OK;
}
Result Manager::synchronize(Storage &storage,const char sn[20],bool active,uint64_t transaction) {
    if(!current.provisioned || !current.snapshot.identity.revision || !sn || memcmp(sn,current.snapshot.identity.product,20)) return current.snapshot.result=Result::WRONG_DEVICE;
    if(current.snapshot.state==rid_activation::State::INVALID) return current.snapshot.result;
    if(!transaction) return current.snapshot.result=Result::INVALID_FORMAT;
    const auto wanted=active?rid_activation::State::ACTIVE:rid_activation::State::UNACTIVATED;
    if(transaction==current.transaction && current.source==Source::PLATFORM) return current.snapshot.result=current.snapshot.state==wanted?Result::OK:Result::VERSION_CONFLICT;
    if(current.generation==UINT32_MAX) return current.snapshot.result=Result::VERSION_CONFLICT;
    State next=current; next.snapshot.state=wanted; next.snapshot.result=Result::OK; next.source=Source::PLATFORM;
    next.transaction=transaction; ++next.generation;
    if(!save(storage,next)) return current.snapshot.result=Result::STORAGE_ERROR;
    return Result::OK;
}
}
