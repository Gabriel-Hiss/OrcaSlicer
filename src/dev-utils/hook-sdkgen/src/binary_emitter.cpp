#include "orca/hook_sdkgen/binary_emitter.hpp"
#include "orca/hook_sdkgen/reader.hpp"
#include "orca/hook_sdkgen/hash_util.hpp"
#ifdef HAVE_ZLIB
#include <zlib.h>
#endif
#include <fstream>
#include <vector>
#include <cstring>
#include <algorithm>
#include <map>
#include <set>
namespace orca::hook_sdkgen {
static void w_u8(std::vector<uint8_t>& o, uint8_t v){ o.push_back(v); }
static void w_u16le(std::vector<uint8_t>& o, uint16_t v){ o.push_back(v & 0xFF); o.push_back((v>>8)&0xFF); }
static void w_u32le(std::vector<uint8_t>& o, uint32_t v){ for(int i=0;i<4;++i) o.push_back((v>>(i*8))&0xFF); }
static void w_u64le(std::vector<uint8_t>& o, uint64_t v){ for(int i=0;i<8;++i) o.push_back((v>>(i*8))&0xFF); }
static void w_bytes(std::vector<uint8_t>& o, const uint8_t* d, size_t n){ o.insert(o.end(), d, d+n); }
static void w_string(std::vector<uint8_t>& o, const std::string& s){ w_u32le(o,(uint32_t)s.size()); if(!s.empty()) w_bytes(o,reinterpret_cast<const uint8_t*>(s.data()), s.size()); }
static uint32_t fnv1a_hash(const std::string& s){ uint32_t h=2166136261u; for(unsigned char c:s){ h^=c; h*=16777619u; } return h; }
void BinaryEmitter::emit(const Manifest& manifest, const std::string& output_path){
    Manifest m=manifest; m.sort_deterministic(); m.recompute_stats();
    std::vector<uint8_t> buf; buf.reserve(1<<20);
    w_bytes(buf,reinterpret_cast<const uint8_t*>("OHBK"),4);
    w_u32le(buf,m.format_version); w_u32le(buf,m.hook_abi);
    w_u8(buf,static_cast<uint8_t>(m.build.os)); w_u8(buf,static_cast<uint8_t>(m.build.arch)); w_u16le(buf,0);
    w_bytes(buf,m.build.image_sha256.data(),32); w_bytes(buf,m.build.debug_guid.data(),16); w_u32le(buf,m.build.debug_age);
    w_string(buf,m.build.debug_file); w_string(buf,m.build.build_id);
    std::map<std::string,uint32_t> pool_index; std::vector<std::string> pool; pool.reserve(m.symbols.size());
    for(auto& s:m.symbols){ std::string k=s.id.empty()?s.decorated_name:s.id; if(k.empty()) k=s.display_name; if(pool_index.find(k)==pool_index.end()){ uint32_t idx=(uint32_t)pool.size(); pool_index[k]=idx; pool.push_back(k); } }
    {
        std::vector<std::string> sorted=pool; std::sort(sorted.begin(),sorted.end()); sorted.erase(std::unique(sorted.begin(),sorted.end()),sorted.end());
        pool=sorted; pool_index.clear(); for(uint32_t i=0;i<pool.size();++i) pool_index[pool[i]]=i;
    }
    std::vector<uint8_t> pool_raw; { std::vector<uint8_t> tmp; w_u32le(tmp,(uint32_t)pool.size()); for(auto& str:pool) w_string(tmp,str); pool_raw=std::move(tmp); }
#ifdef HAVE_ZLIB
    std::vector<uint8_t> pool_comp; bool use_comp=false; {
        uLongf b=compressBound((uLong)pool_raw.size()); pool_comp.resize(b); uLongf cs=b;
        int rc=compress2(pool_comp.data(),&cs,pool_raw.data(),(uLong)pool_raw.size(),Z_BEST_COMPRESSION);
        if(rc==Z_OK && cs+9 < pool_raw.size()){ pool_comp.resize(cs); use_comp=true; }
    }
    if(use_comp){ w_u8(buf,1); w_u32le(buf,(uint32_t)pool_raw.size()); w_u32le(buf,(uint32_t)pool_comp.size()); w_bytes(buf,pool_comp.data(),pool_comp.size()); }
    else { w_u8(buf,0); w_bytes(buf,pool_raw.data(),pool_raw.size()); }
#else
    w_u8(buf,0); w_bytes(buf,pool_raw.data(),pool_raw.size());
#endif
    w_u32le(buf,(uint32_t)m.symbols.size());
    for(auto& s:m.symbols){
        std::string k=s.id.empty()?s.decorated_name:s.id; if(k.empty()) k=s.display_name;
        uint32_t idx=pool_index[k]; uint32_t h=fnv1a_hash(k);
        w_u32le(buf,idx); w_u32le(buf,h); w_u32le(buf,(uint32_t)s.rva); w_u32le(buf,s.size);
        w_u8(buf,static_cast<uint8_t>(s.kind)); w_u8(buf,0); w_u16le(buf,0);
        w_u16le(buf,0);
    }
    auto ph=sha256(buf.data(),buf.size()); w_bytes(buf,ph.data(),32);
    std::ofstream out(output_path,std::ios::binary); if(!out) throw GenerationError("cannot open binary for write: "+output_path);
    out.write(reinterpret_cast<const char*>(buf.data()),buf.size()); if(!out) throw GenerationError("binary write failed: "+output_path);
}
bool BinaryEmitter::validate_file(const std::string& p, std::string* e){
    std::ifstream f(p,std::ios::binary); if(!f){ if(e) *e="cannot open"; return false; }
    f.seekg(0,std::ios::end); size_t s=(size_t)f.tellg(); f.seekg(0,std::ios::beg);
    if(s<4+4+4+32){ if(e) *e="too small"; return false; }
    std::vector<uint8_t> d(s); f.read((char*)d.data(),s);
    if(std::memcmp(d.data(),"OHBK",4)!=0){ if(e) *e="bad magic"; return false; }
    if(s<32) return false; size_t pl=s-32; auto ex=sha256(d.data(),pl);
    if(std::memcmp(ex.data(),d.data()+pl,32)!=0){ if(e) *e="SHA256 footer mismatch"; return false; }
    return true;
}
} // namespace
