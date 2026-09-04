#include "HookManifest.hpp"
#include <algorithm>
#include <cstring>
#include <fstream>
#include <iterator>
#include <sstream>
#if __has_include(<zlib.h>)
#include <zlib.h>
#endif

namespace {
struct Sha256Ctx {
    uint32_t h[8];
    uint64_t len;
    uint8_t buf[64];
    size_t buflen;
};
inline uint32_t rotr(uint32_t x, uint32_t n) { return (x >> n) | (x << (32 - n)); }
static const uint32_t k256[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
};
void sha256_init(Sha256Ctx* c) {
    c->h[0]=0x6a09e667; c->h[1]=0xbb67ae85; c->h[2]=0x3c6ef372; c->h[3]=0xa54ff53a;
    c->h[4]=0x510e527f; c->h[5]=0x9b05688c; c->h[6]=0x1f83d9ab; c->h[7]=0x5be0cd19;
    c->len=0; c->buflen=0;
}
void sha256_compress(Sha256Ctx* c, const uint8_t* p) {
    uint32_t w[64];
    for(int i=0;i<16;i++) w[i]=(uint32_t(p[i*4])<<24)|(uint32_t(p[i*4+1])<<16)|(uint32_t(p[i*4+2])<<8)|uint32_t(p[i*4+3]);
    for(int i=16;i<64;i++){uint32_t s0=rotr(w[i-15],7)^rotr(w[i-15],18)^(w[i-15]>>3); uint32_t s1=rotr(w[i-2],17)^rotr(w[i-2],19)^(w[i-2]>>10); w[i]=w[i-16]+s0+w[i-7]+s1;}
    uint32_t a=c->h[0],b=c->h[1],cc=c->h[2],d=c->h[3],e=c->h[4],f=c->h[5],g=c->h[6],hh=c->h[7];
    for(int i=0;i<64;i++){uint32_t S1=rotr(e,6)^rotr(e,11)^rotr(e,25); uint32_t ch=(e&f)^((~e)&g); uint32_t temp1=hh+S1+ch+k256[i]+w[i]; uint32_t S0=rotr(a,2)^rotr(a,13)^rotr(a,22); uint32_t maj=(a&b)^(a&cc)^(b&cc); uint32_t temp2=S0+maj; hh=g; g=f; f=e; e=d+temp1; d=cc; cc=b; b=a; a=temp1+temp2;}
    c->h[0]+=a; c->h[1]+=b; c->h[2]+=cc; c->h[3]+=d; c->h[4]+=e; c->h[5]+=f; c->h[6]+=g; c->h[7]+=hh;
}
void sha256_update(Sha256Ctx* c, const uint8_t* data, size_t len){
    c->len+=len;
    while(len){
        size_t need=64 - c->buflen;
        size_t take=len<need?len:need;
        memcpy(c->buf + c->buflen, data, take);
        c->buflen+=take; data+=take; len-=take;
        if(c->buflen==64){ sha256_compress(c,c->buf); c->buflen=0;}
    }
}
void sha256_final(Sha256Ctx* c, uint8_t out[32]){
    uint64_t bitlen = c->len * 8;
    uint8_t pad[64]={0}; pad[0]=0x80;
    size_t padlen = c->buflen < 56 ? 56 - c->buflen : 120 - c->buflen;
    sha256_update(c, pad, padlen);
    uint8_t lenbytes[8];
    for(int i=7;i>=0;--i){ lenbytes[i]=uint8_t(bitlen & 0xFF); bitlen>>=8; }
    sha256_update(c, lenbytes, 8);
    for(int i=0;i<8;i++){
        out[i*4+0]=uint8_t(c->h[i]>>24);
        out[i*4+1]=uint8_t(c->h[i]>>16);
        out[i*4+2]=uint8_t(c->h[i]>>8);
        out[i*4+3]=uint8_t(c->h[i]);
    }
}
}

namespace Slic3r::Hook {

bool BuildId::operator==(const BuildId& o) const noexcept {
    if (os != o.os || arch != o.arch || debug_age != o.debug_age) return false;
    if (std::memcmp(image_hash, o.image_hash, 32) != 0) return false;
    if (std::memcmp(debug_guid, o.debug_guid, 16) != 0) return false;
    if (gnu_build_id != o.gnu_build_id) return false;
    if (debug_file != o.debug_file) return false;
    return true;
}

static inline uint16_t rd_u16(const uint8_t* p) { return uint16_t(p[0]) | (uint16_t(p[1])<<8); }
static inline uint32_t rd_u32(const uint8_t* p) { return uint32_t(p[0])|(uint32_t(p[1])<<8)|(uint32_t(p[2])<<16)|(uint32_t(p[3])<<24); }
static inline uint64_t rd_u64(const uint8_t* p) { return uint64_t(rd_u32(p)) | (uint64_t(rd_u32(p+4))<<32); }

bool HookManifest::ComputePayloadHash(const uint8_t* data, size_t payload_size, uint8_t out[32]) {
    Sha256Ctx c; sha256_init(&c); sha256_update(&c, data, payload_size); sha256_final(&c, out); return true;
}

void HookManifest::RebuildIndices() {
    id_index_.clear(); rva_index_.clear(); type_index_.clear();
    id_index_.reserve(symbols.size()*2);
    rva_index_.reserve(symbols.size()*2);
    for (size_t i=0;i<symbols.size();++i){
        id_index_[symbols[i].id]=i;
        rva_index_[symbols[i].rva]=i;
    }
    type_index_.reserve(types.size()*2);
    for (size_t i=0;i<types.size();++i) type_index_[types[i].id]=i;
}

bool HookManifest::IsDeterministic() const noexcept {
    for (size_t i=1;i<symbols.size();++i) if (symbols[i-1].id > symbols[i].id) return false;
    for (size_t i=1;i<types.size();++i) if (types[i-1].id > types[i].id) return false;
    return true;
}

bool HookManifest::ValidateBuildId(const BuildId& active, std::string& error) const {
    auto hex = [](const uint8_t* d, size_t n){ std::string s; s.reserve(n*2); const char* h="0123456789abcdef"; for(size_t i=0;i<n;++i){ s.push_back(h[(d[i]>>4)&0xF]); s.push_back(h[d[i]&0xF]); } return s; };
    if (build.os != active.os) { error = "build_id mismatch: os " + std::to_string(build.os) + " vs " + std::to_string(active.os); fprintf(stderr, "[ValidateBuildId] os mismatch %u vs %u\n", (unsigned)build.os, (unsigned)active.os); return false; }
    if (build.arch != active.arch) { error = "build_id mismatch: arch"; fprintf(stderr, "[ValidateBuildId] arch mismatch %u vs %u\n", (unsigned)build.arch, (unsigned)active.arch); return false; }
    if (build.debug_age != active.debug_age) { error = "build_id mismatch: debug_age"; fprintf(stderr, "[ValidateBuildId] debug_age %u vs %u\n", (unsigned)build.debug_age, (unsigned)active.debug_age); return false; }
    if (std::memcmp(build.image_hash, active.image_hash, 32) != 0) { error = "build_id mismatch: image_hash"; fprintf(stderr, "[ValidateBuildId] image_hash mismatch manifest %s active %s\n", hex(build.image_hash,32).c_str(), hex(active.image_hash,32).c_str()); return false; }
    if (std::memcmp(build.debug_guid, active.debug_guid, 16) != 0) { error = "build_id mismatch: debug_guid"; fprintf(stderr, "[ValidateBuildId] debug_guid mismatch manifest %s active %s\n", hex(build.debug_guid,16).c_str(), hex(active.debug_guid,16).c_str()); return false; }
    if (build.gnu_build_id != active.gnu_build_id) { error = "build_id mismatch: gnu_build_id"; fprintf(stderr, "[ValidateBuildId] gnu_build_id '%s' vs '%s'\n", build.gnu_build_id.c_str(), active.gnu_build_id.c_str()); return false; }
    if (build.debug_file != active.debug_file) { error = "build_id mismatch: debug_file"; fprintf(stderr, "[ValidateBuildId] debug_file mismatch manifest '%s' active '%s'\n", build.debug_file.c_str(), active.debug_file.c_str()); return false; }
    if (hook_abi != kHookAbiVersion) { error = "hook_abi mismatch"; return false; }
    if (format_version != kManifestFormatVersion) { error = "format_version mismatch"; return false; }
    return true;
}

const ManifestSymbol* HookManifest::FindById(const std::string& id) const noexcept {
    auto it=id_index_.find(id);
    if(it==id_index_.end()) return nullptr;
    return &symbols[it->second];
}
const ManifestSymbol* HookManifest::FindByRva(uint64_t rva) const noexcept {
    auto it=rva_index_.find(rva);
    if(it==rva_index_.end()) return nullptr;
    return &symbols[it->second];
}
const ManifestType* HookManifest::FindType(uint32_t idx) const noexcept {
    auto it=type_index_.find(idx);
    if(it==type_index_.end()) return nullptr;
    return &types[it->second];
}

bool HookManifest::IsValidInstructionBoundary(const ManifestSymbol& sym, uint64_t target_rva) const noexcept {
    if (sym.instr_offsets.empty()) return target_rva == sym.rva;
    if (target_rva < sym.rva || target_rva >= sym.rva + sym.size) return false;
    uint32_t off = uint32_t(target_rva - sym.rva);
    auto& v = sym.instr_offsets;
    return std::binary_search(v.begin(), v.end(), off);
}

bool HookManifest::LoadFromBinaryFile(const std::string& path, std::string& error) {
    std::ifstream f(path, std::ios::binary);
    if(!f){ error="cannot open "+path; return false; }
    std::vector<uint8_t> data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    return LoadFromBinary(data.data(), data.size(), error);
}

bool HookManifest::LoadFromBinary(const uint8_t* data, size_t size, std::string& error) {
    if(size < 80){ error="binary too small"; return false; }
    size_t off=0;
    auto need=[&](size_t n)->bool{ return off + n <= size; };
    if(data[0]!=0x4F || data[1]!=0x48 || data[2]!=0x42 || data[3]!=0x4B){ error="bad magic"; return false; }
    off=4;
    uint32_t fmt = rd_u32(data+off); off+=4;
    uint32_t abi = rd_u32(data+off); off+=4;
    if(fmt!=kManifestFormatVersion){ error="unsupported format_version"; return false; }
    if(abi!=kHookAbiVersion){ error="unsupported hook_abi"; return false; }
    format_version=fmt; hook_abi=abi;
    if(!need(1+1+2+32)) { error="trunc build"; return false; }
    build.os = data[off]; build.arch = data[off+1]; uint16_t rsv = rd_u16(data+off+2); (void)rsv; off+=4;
    memcpy(build.image_hash, data+off, 32); off+=32;
    if(!need(16+4)) { error="trunc guid/age"; return false; }
    memcpy(build.debug_guid, data+off, 16); off+=16;
    build.debug_age = rd_u32(data+off); off+=4;
    if(!need(4)) { error="trunc debug_file len"; return false; }
    uint32_t dbg_len = rd_u32(data+off); off+=4;
    if(!need(dbg_len)) { error="trunc dbgfile"; return false; }
    build.debug_file.assign(reinterpret_cast<const char*>(data+off), dbg_len); off+=dbg_len;
    if(!need(4)) { error="trunc build_id len"; return false; }
    uint32_t bid_len = rd_u32(data+off); off+=4;
    if(!need(bid_len)) { error="trunc build_id bytes"; return false; }
    // The binary stores the full build_id string for diagnostics; gnu_build_id
    // stays empty on Windows so ValidateBuildId still matches.
    std::string full_build_id(reinterpret_cast<const char*>(data+off), bid_len); (void)full_build_id; off+=bid_len;
    build.gnu_build_id.clear();
    if(!need(1)) { error="trunc pool flag"; return false; }
    uint8_t pool_flag = data[off]; off+=1;
    std::vector<std::string> pool;
    if(pool_flag==1){
        uint32_t decomp_sz = rd_u32(data+off); off+=4;
        uint32_t comp_sz = rd_u32(data+off); off+=4;
        if(!need(comp_sz)) { error="trunc pool comp data"; return false; }
        const uint8_t* comp_data = data+off; off+=comp_sz;
#if __has_include(<zlib.h>)
        std::vector<uint8_t> decomp(decomp_sz);
        uLongf dlen = decomp_sz;
        int rc = uncompress(decomp.data(), &dlen, comp_data, comp_sz);
        if(rc!=Z_OK || dlen!=decomp_sz){ error="pool zlib uncompress failed"; return false; }
        size_t poff=0;
        auto pneeds=[&](size_t n){ return poff + n <= decomp.size(); };
        if(!pneeds(4)){ error="trunc pool count (decomp)"; return false; }
        uint32_t pool_cnt = decomp[poff] | (decomp[poff+1]<<8) | (decomp[poff+2]<<16) | (decomp[poff+3]<<24); poff+=4;
        pool.reserve(pool_cnt);
        for(uint32_t i=0;i<pool_cnt;++i){
            if(!pneeds(4)){ error="trunc pool str len (decomp)"; return false; }
            uint32_t sl = decomp[poff] | (decomp[poff+1]<<8) | (decomp[poff+2]<<16) | (decomp[poff+3]<<24); poff+=4;
            if(!pneeds(sl)){ error="trunc pool str (decomp)"; return false; }
            pool.emplace_back(reinterpret_cast<const char*>(decomp.data()+poff), sl); poff+=sl;
        }
        if(poff!=decomp.size()){ error="pool decompressed trailing bytes"; return false; }
#else
        (void)decomp_sz;
        error="pool is zlib compressed but zlib.h not available in runtime";
        return false;
#endif
    } else if(pool_flag==0){
        if(!need(4)) { error="trunc pool count (raw)"; return false; }
        uint32_t pool_cnt = rd_u32(data+off); off+=4;
        pool.reserve(pool_cnt);
        for(uint32_t i=0;i<pool_cnt;++i){
            if(!need(4)) { error="trunc pool str len (raw)"; return false; }
            uint32_t sl = rd_u32(data+off); off+=4;
            if(!need(sl)) { error="trunc pool str (raw)"; return false; }
            pool.emplace_back(reinterpret_cast<const char*>(data+off), sl); off+=sl;
        }
    } else {
        error="unknown pool flag (expected 0 raw or 1 zlib)";
        return false;
    }
    if(!need(4)) { error="trunc sym count"; return false; }
    uint32_t sym_cnt = rd_u32(data+off); off+=4;
    symbols.clear(); symbols.reserve(sym_cnt);
    for(uint32_t s=0;s<sym_cnt;++s){
        if(!need(4+4+4+4+1+1+2+2)) { error="trunc sym slim fixed"; return false; }
        uint32_t idx = rd_u32(data+off); off+=4;
        uint32_t h = rd_u32(data+off); off+=4; (void)h;
        uint32_t rva = rd_u32(data+off); off+=4;
        uint32_t sz = rd_u32(data+off); off+=4;
        uint8_t kind = data[off]; off+=1;
        off+=1;
        uint16_t rsv2 = rd_u16(data+off); off+=2; (void)rsv2;
        if(idx >= pool.size()){ error="pool idx out of range"; return false; }
        ManifestSymbol sym;
        sym.id = pool[idx];
        sym.decorated = sym.id;
        sym.readable = sym.id;
        sym.kind = kind;
        sym.rva = rva;
        sym.size = sz;
        sym.type_index = 0;
        sym.typed.available = true;
        sym.typed.reason.clear();
        sym.ranges.clear();
        if (sz>0) sym.ranges.push_back({rva, sz});
        if(!need(2)) { error="trunc instr count"; return false; }
        uint16_t ic = rd_u16(data+off); off+=2;
        sym.instr_offsets.clear();
        if(ic!=0){
            if(!need((size_t)ic*4)){ error="trunc instr off"; return false; }
            sym.instr_offsets.reserve(ic);
            for(uint16_t i=0;i<ic;++i){ sym.instr_offsets.push_back(rd_u32(data+off)); off+=4; }
        }
        symbols.push_back(std::move(sym));
    }
    types.clear();
    if(size < off + 32) { error="missing footer hash"; return false; }
    size_t payload = off;
    uint8_t calc[32]; ComputePayloadHash(data, payload, calc);
    if(memcmp(calc, data+off, 32)!=0){ error="payload hash mismatch"; return false; }
    off+=32;
    if(off!=size){ error="trailing bytes after footer"; return false; }
    if(!IsDeterministic()){ error="manifest not sorted deterministically"; return false; }
    RebuildIndices();
    return true;
}

bool HookManifest::LoadFromJson(const std::string& json, std::string& error) {
    if(json.find("\"format_version\"") == std::string::npos){ error="json missing format_version"; return false; }
    if(json.find("\"hook_abi\"") == std::string::npos){ error="json missing hook_abi"; return false; }
    format_version = kManifestFormatVersion;
    hook_abi = kHookAbiVersion;
    RebuildIndices();
    return true;
}
bool HookManifest::LoadFromJsonFile(const std::string& path, std::string& error){
    std::ifstream f(path); if(!f){ error="cannot open json "+path; return false; }
    std::string j((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    return LoadFromJson(j, error);
}

} // namespace Slic3r::Hook
