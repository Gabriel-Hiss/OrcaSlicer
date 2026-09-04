#include "orca/hook_sdkgen/manifest_emitter.hpp"
#include "orca/hook_sdkgen/reader.hpp"
#include "orca/hook_sdkgen/hash_util.hpp"
#include <fstream>
#include <sstream>
#include <algorithm>
#include <iomanip>
#include <vector>
#ifdef HAVE_ZLIB
#include <zlib.h>
#endif
namespace orca::hook_sdkgen {

static std::string json_escape(const std::string& s) {
    std::string out; out.reserve(s.size()+8);
    for (unsigned char c : s) {
        switch(c){
            case '\"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) { char buf[7]; std::snprintf(buf,sizeof(buf),"\\u%04x",c); out+=buf; }
                else out+= (char)c;
        }
    }
    return out;
}

#ifdef HAVE_ZLIB
class GzStreamWriter {
    std::ofstream file;
    z_stream strm{};
    uint32_t crc = 0;
    uint32_t total_in = 0;
    std::string uncompressed_buffer; // for correct CRC/ISIZE verification
    bool ok = false;
public:
    explicit GzStreamWriter(const std::string& path) {
        file.open(path, std::ios::binary);
        if (!file) throw GenerationError("cannot open manifest for write: " + path);
        uint8_t hdr[10] = {0x1f,0x8b,0x08,0x00,0x00,0x00,0x00,0x00,0x00,0xff};
        file.write(reinterpret_cast<char*>(hdr), 10);
        if (!file) throw GenerationError("failed to write gzip header: " + path);
        crc = crc32(0L, Z_NULL, 0);
        strm.zalloc = Z_NULL; strm.zfree = Z_NULL; strm.opaque = Z_NULL;
        if (deflateInit2(&strm, Z_BEST_COMPRESSION, Z_DEFLATED, -15, 8, Z_DEFAULT_STRATEGY) != Z_OK)
            throw GenerationError("deflateInit2 failed");
        ok = true;
        uncompressed_buffer.reserve(1<<20);
    }
    void write(const char* data, size_t len) {
        if (!ok || len==0) return;
        uncompressed_buffer.append(data, len);
        // Incremental CRC with chunking for len > UINT_MAX
        size_t remaining = len;
        const Bytef* p = reinterpret_cast<const Bytef*>(data);
        while (remaining > 0) {
            uInt chunk = remaining > UINT_MAX ? UINT_MAX : (uInt)remaining;
            crc = crc32(crc, p, chunk);
            p += chunk;
            remaining -= chunk;
        }
        total_in += (uint32_t)len;
        strm.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(data));
        strm.avail_in = (uInt)len;
        // Handle len > UINT_MAX by looping (though len is small for manifest, this is for correctness)
        size_t in_remaining = len;
        const char* in_ptr = data;
        while (in_remaining > 0) {
            uInt chunk = in_remaining > UINT_MAX ? UINT_MAX : (uInt)in_remaining;
            strm.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(in_ptr));
            strm.avail_in = chunk;
            in_ptr += chunk;
            in_remaining -= chunk;
            while (strm.avail_in > 0) {
                unsigned char out[1<<15];
                strm.next_out = out;
                strm.avail_out = sizeof(out);
                int ret = deflate(&strm, Z_NO_FLUSH);
                if (ret == Z_STREAM_ERROR) throw GenerationError("deflate failed");
                size_t have = sizeof(out) - strm.avail_out;
                if (have) {
                    file.write(reinterpret_cast<char*>(out), have);
                    if (!file) throw GenerationError("gzip write failed");
                }
            }
        }
    }
    void write(const std::string& s) { write(s.c_str(), s.size()); }
    void finish() {
        if (!ok) return;
        unsigned char out[1<<15];
        int ret;
        do {
            strm.next_out = out;
            strm.avail_out = sizeof(out);
            ret = deflate(&strm, Z_FINISH);
            if (ret == Z_STREAM_ERROR) throw GenerationError("deflate finish failed");
            size_t have = sizeof(out) - strm.avail_out;
            if (have) {
                file.write(reinterpret_cast<char*>(out), have);
                if (!file) throw GenerationError("gzip finish write failed");
            }
        } while (ret != Z_STREAM_END);
        // CRC32 and ISIZE are computed from the uncompressed buffer.
        uint32_t correct_crc = crc32(0L, Z_NULL, 0);
        size_t rem = uncompressed_buffer.size();
        const Bytef* p = reinterpret_cast<const Bytef*>(uncompressed_buffer.data());
        while (rem > 0) {
            uInt chunk = rem > UINT_MAX ? UINT_MAX : (uInt)rem;
            correct_crc = crc32(correct_crc, p, chunk);
            p += chunk;
            rem -= chunk;
        }
        uint32_t correct_isize = (uint32_t)uncompressed_buffer.size();
        if (crc != correct_crc || total_in != correct_isize) {
            crc = correct_crc;
            total_in = correct_isize;
        }
        if (total_in != (uint32_t)strm.total_in) {
            total_in = (uint32_t)strm.total_in;
        }
        deflateEnd(&strm);
        ok = false;
        uint8_t footer[8];
        footer[0]= crc & 0xFF; footer[1]=(crc>>8)&0xFF; footer[2]=(crc>>16)&0xFF; footer[3]=(crc>>24)&0xFF;
        footer[4]= total_in & 0xFF; footer[5]=(total_in>>8)&0xFF; footer[6]=(total_in>>16)&0xFF; footer[7]=(total_in>>24)&0xFF;
        file.write(reinterpret_cast<char*>(footer), 8);
        if (!file) throw GenerationError("failed to write gzip footer");
        file.close();
    }
    ~GzStreamWriter() { if (ok) { try{ finish(); }catch(...){} } }
};
#endif

class FileStreamWriter {
    std::ofstream& out;
public:
    explicit FileStreamWriter(std::ofstream& o) : out(o) {}
    void write(const char* data, size_t len) { out.write(data, len); if(!out) throw GenerationError("write failed"); }
    void write(const std::string& s) { write(s.c_str(), s.size()); }
    void finish() {}
};

void ManifestEmitter::emit(const Manifest& manifest, const std::string& output_path) {
    Manifest m = manifest;
    m.sort_deterministic();
    m.recompute_stats();
    bool is_gz = output_path.size()>=3 && output_path.substr(output_path.size()-3)==".gz";
#ifdef HAVE_ZLIB
    if (is_gz) {
        GzStreamWriter gz(output_path);
        auto w = [&](const char* d, size_t n){ gz.write(d,n); };
        auto ws = [&](const std::string& s){ gz.write(s.c_str(), s.size()); };
        auto wi = [&](int lvl){ for(int i=0;i<lvl;++i) gz.write("  ",2); };
        gz.write("{\n",2);
        wi(1); ws("\"format_version\": "); ws(std::to_string(m.format_version)); ws(",\n");
        wi(1); ws("\"hook_abi\": "); ws(std::to_string(m.hook_abi)); ws(",\n");
        wi(1); ws("\"build\": {\n");
        wi(2); ws("\"os\": \""); ws(os_to_string(m.build.os)); ws("\",\n");
        wi(2); ws("\"arch\": \""); ws(arch_to_string(m.build.arch)); ws("\",\n");
        wi(2); ws("\"debug_file\": \""); ws(json_escape(m.build.debug_file)); ws("\",\n");
        wi(2); ws("\"debug_guid\": \""); ws(m.build.debug_guid_string()); ws("\",\n");
        wi(2); ws("\"debug_age\": "); ws(std::to_string(m.build.debug_age)); ws(",\n");
        wi(2); ws("\"image_sha256\": \""); ws(m.build.image_sha256_hex()); ws("\",\n");
        wi(2); ws("\"build_id\": \""); ws(json_escape(m.build.build_id)); ws("\"\n");
        wi(1); ws("},\n");
        wi(1); ws("\"types\": [\n");
        for(size_t i=0;i<m.types.size();++i){
            auto &t=m.types[i];
            wi(2); ws("{\n");
            wi(3); ws("\"id\": "); ws(std::to_string(t.id)); ws(",\n");
            wi(3); ws("\"kind\": \""); ws(type_kind_to_string(t.kind)); ws("\",\n");
            wi(3); ws("\"name\": \""); ws(json_escape(t.name)); ws("\",\n");
            wi(3); ws("\"size\": "); ws(std::to_string(t.size)); ws(",\n");
            wi(3); ws("\"align\": "); ws(std::to_string(t.align));
            bool has_fields = !t.fields.empty();
            bool has_enum = !t.enum_values.empty();
            bool has_pointee = t.pointee_type!=0;
            if(has_fields||has_enum||has_pointee) ws(",\n"); else ws("\n");
            if(has_fields){
                wi(3); ws("\"fields\": [\n");
                for(size_t fi=0;fi<t.fields.size();++fi){
                    wi(4); ws("{\"name\":\""); ws(json_escape(t.fields[fi].name)); ws("\",\"type\":"); ws(std::to_string(t.fields[fi].type_id)); ws(",\"offset\":"); ws(std::to_string(t.fields[fi].offset)); ws("}");
                    if(fi+1<t.fields.size()) ws(",");
                    ws("\n");
                }
                wi(3); ws("]");
                if(has_enum||has_pointee) ws(",\n"); else ws("\n");
            }
            if(has_enum){
                wi(3); ws("\"enum_values\": {\n");
                size_t ei=0;
                for(auto &kv: t.enum_values){
                    wi(4); ws("\""); ws(json_escape(kv.second)); ws("\": "); ws(std::to_string(kv.first));
                    if(ei+1<t.enum_values.size()) ws(",");
                    ws("\n"); ++ei;
                }
                wi(3); ws("}");
                if(has_pointee) ws(",\n"); else ws("\n");
            }
            if(has_pointee){
                wi(3); ws("\"pointee_type\": "); ws(std::to_string(t.pointee_type));
                if(t.array_count!=0) ws(",\n"); else ws("\n");
            }
            if(t.array_count!=0){
                wi(3); ws("\"array_count\": "); ws(std::to_string(t.array_count)); ws("\n");
            }
            wi(2); ws("}");
            if(i+1<m.types.size()) ws(",");
            ws("\n");
        }
        wi(1); ws("],\n");
        wi(1); ws("\"symbols\": [\n");
        for(size_t i=0;i<m.symbols.size();++i){
            auto &s=m.symbols[i];
            std::string canonical = !s.decorated_name.empty() ? s.decorated_name : s.display_name;
            if(canonical.empty()) canonical = s.id;
            wi(2); ws("{\n");
            wi(3); ws("\"id\": \""); ws(json_escape(s.id)); ws("\",\n");
            if(canonical != s.id){
                wi(3); ws("\"name\": \""); ws(json_escape(canonical)); ws("\",\n");
            }
            if(!s.display_name.empty() && s.display_name != canonical && s.display_name != s.id){
                wi(3); ws("\"display_name\": \""); ws(json_escape(s.display_name)); ws("\",\n");
            }
            wi(3); ws("\"kind\": \""); ws(symbol_kind_to_string(s.kind)); ws("\",\n");
            wi(3); ws("\"rva\": "); ws(std::to_string(s.rva)); ws(",\n");
            wi(3); ws("\"size\": "); ws(std::to_string(s.size)); ws(",\n");
            wi(3); ws("\"type\": "); ws(std::to_string(s.type_id)); ws(",\n");
            if(!s.calling_convention.empty() && s.calling_convention!="unknown"){
                wi(3); ws("\"calling_convention\": \""); ws(json_escape(s.calling_convention)); ws("\",\n");
            }
            if(!s.section.empty()){
                wi(3); ws("\"section\": \""); ws(json_escape(s.section)); ws("\",\n");
            }
            if(s.ranges.size()!=1 || s.ranges.empty() || s.ranges[0].rva != s.rva || s.ranges[0].size != s.size){
                wi(3); ws("\"ranges\": [");
                for(size_t ri=0;ri<s.ranges.size();++ri){ if(ri) ws(", "); ws("{\"rva\":"); ws(std::to_string(s.ranges[ri].rva)); ws(",\"size\":"); ws(std::to_string(s.ranges[ri].size)); ws("}"); }
                ws("],\n");
            }
            if(!s.instructions.empty()){
                wi(3); ws("\"instructions\": [");
                for(size_t ii=0;ii<s.instructions.size();++ii){ if(ii) ws(", "); ws("{\"offset\":"); ws(std::to_string(s.instructions[ii].offset)); ws(",\"size\":"); ws(std::to_string((int)s.instructions[ii].size)); ws(",\"is_call\":"); ws(s.instructions[ii].is_call?"true":"false"); ws(",\"is_ret\":"); ws(s.instructions[ii].is_ret?"true":"false"); ws("}"); }
                ws("],\n");
            }
            if(s.source){
                wi(3); ws("\"source\": {\"file\":\""); ws(json_escape(s.source->file)); ws("\",\"line\":"); ws(std::to_string(s.source->line)); ws("},\n");
            }
            wi(3); ws("\"typed_binding\": {\"available\":"); ws(s.typed_binding.available?"true":"false"); ws(",\"reason\":\""); ws(json_escape(s.typed_binding.reason)); ws("\"}\n");
            wi(2); ws("}");
            if(i+1<m.symbols.size()) ws(",");
            ws("\n");
        }
        wi(1); ws("],\n");
        wi(1); ws("\"stats\": {\n");
        wi(2); ws("\"total_symbols\": "); ws(std::to_string(m.stats.total_symbols)); ws(",\n");
        wi(2); ws("\"typed_available\": "); ws(std::to_string(m.stats.typed_available)); ws(",\n");
        wi(2); ws("\"raw_only\": "); ws(std::to_string(m.stats.raw_only)); ws(",\n");
        wi(2); ws("\"inlined_or_no_address\": "); ws(std::to_string(m.stats.inlined_or_no_address)); ws(",\n");
        wi(2); ws("\"functions\": "); ws(std::to_string(m.stats.functions)); ws(",\n");
        wi(2); ws("\"data\": "); ws(std::to_string(m.stats.data)); ws(",\n");
        wi(2); ws("\"vtables\": "); ws(std::to_string(m.stats.vtables)); ws(",\n");
        wi(2); ws("\"imports\": "); ws(std::to_string(m.stats.imports)); ws("\n");
        wi(1); ws("}\n");
        ws("}\n");
        gz.finish();
        return;
    }
#endif
    // Fallback: plain file streaming without building huge string
    {
        std::ofstream out(output_path, std::ios::binary);
        if(!out) throw GenerationError("cannot open manifest for write: " + output_path);
        auto w = [&](const char* d, size_t n){ out.write(d,n); if(!out) throw GenerationError("write failed"); };
        auto ws = [&](const std::string& s){ w(s.c_str(), s.size()); };
        auto wi = [&](int lvl){ for(int i=0;i<lvl;++i) w("  ",2); };
        w("{\n",2);
        wi(1); ws("\"format_version\": "); ws(std::to_string(m.format_version)); ws(",\n");
        wi(1); ws("\"hook_abi\": "); ws(std::to_string(m.hook_abi)); ws(",\n");
        wi(1); ws("\"build\": {\n");
        wi(2); ws("\"os\": \""); ws(os_to_string(m.build.os)); ws("\",\n");
        wi(2); ws("\"arch\": \""); ws(arch_to_string(m.build.arch)); ws("\",\n");
        wi(2); ws("\"debug_file\": \""); ws(json_escape(m.build.debug_file)); ws("\",\n");
        wi(2); ws("\"debug_guid\": \""); ws(m.build.debug_guid_string()); ws("\",\n");
        wi(2); ws("\"debug_age\": "); ws(std::to_string(m.build.debug_age)); ws(",\n");
        wi(2); ws("\"image_sha256\": \""); ws(m.build.image_sha256_hex()); ws("\",\n");
        wi(2); ws("\"build_id\": \""); ws(json_escape(m.build.build_id)); ws("\"\n");
        wi(1); ws("},\n");
        wi(1); ws("\"types\": [\n");
        for(size_t i=0;i<m.types.size();++i){
            auto &t=m.types[i];
            wi(2); ws("{\n");
            wi(3); ws("\"id\": "); ws(std::to_string(t.id)); ws(",\n");
            wi(3); ws("\"kind\": \""); ws(type_kind_to_string(t.kind)); ws("\",\n");
            wi(3); ws("\"name\": \""); ws(json_escape(t.name)); ws("\",\n");
            wi(3); ws("\"size\": "); ws(std::to_string(t.size)); ws(",\n");
            wi(3); ws("\"align\": "); ws(std::to_string(t.align));
            bool has_fields = !t.fields.empty();
            bool has_enum = !t.enum_values.empty();
            bool has_pointee = t.pointee_type!=0;
            if(has_fields||has_enum||has_pointee) ws(",\n"); else ws("\n");
            if(has_fields){
                wi(3); ws("\"fields\": [\n");
                for(size_t fi=0;fi<t.fields.size();++fi){
                    wi(4); ws("{\"name\":\""); ws(json_escape(t.fields[fi].name)); ws("\",\"type\":"); ws(std::to_string(t.fields[fi].type_id)); ws(",\"offset\":"); ws(std::to_string(t.fields[fi].offset)); ws("}");
                    if(fi+1<t.fields.size()) ws(",");
                    ws("\n");
                }
                wi(3); ws("]");
                if(has_enum||has_pointee) ws(",\n"); else ws("\n");
            }
            if(has_enum){
                wi(3); ws("\"enum_values\": {\n");
                size_t ei=0;
                for(auto &kv: t.enum_values){
                    wi(4); ws("\""); ws(json_escape(kv.second)); ws("\": "); ws(std::to_string(kv.first));
                    if(ei+1<t.enum_values.size()) ws(",");
                    ws("\n"); ++ei;
                }
                wi(3); ws("}");
                if(has_pointee) ws(",\n"); else ws("\n");
            }
            if(has_pointee){
                wi(3); ws("\"pointee_type\": "); ws(std::to_string(t.pointee_type));
                if(t.array_count!=0) ws(",\n"); else ws("\n");
            }
            if(t.array_count!=0){
                wi(3); ws("\"array_count\": "); ws(std::to_string(t.array_count)); ws("\n");
            }
            wi(2); ws("}");
            if(i+1<m.types.size()) ws(",");
            ws("\n");
        }
        wi(1); ws("],\n");
        wi(1); ws("\"symbols\": [\n");
        for(size_t i=0;i<m.symbols.size();++i){
            auto &s=m.symbols[i];
            std::string canonical = !s.decorated_name.empty() ? s.decorated_name : s.display_name;
            if(canonical.empty()) canonical = s.id;
            wi(2); ws("{\n");
            wi(3); ws("\"id\": \""); ws(json_escape(s.id)); ws("\",\n");
            if(canonical != s.id){
                wi(3); ws("\"name\": \""); ws(json_escape(canonical)); ws("\",\n");
            }
            if(!s.display_name.empty() && s.display_name != canonical && s.display_name != s.id){
                wi(3); ws("\"display_name\": \""); ws(json_escape(s.display_name)); ws("\",\n");
            }
            wi(3); ws("\"kind\": \""); ws(symbol_kind_to_string(s.kind)); ws("\",\n");
            wi(3); ws("\"rva\": "); ws(std::to_string(s.rva)); ws(",\n");
            wi(3); ws("\"size\": "); ws(std::to_string(s.size)); ws(",\n");
            wi(3); ws("\"type\": "); ws(std::to_string(s.type_id)); ws(",\n");
            if(!s.calling_convention.empty() && s.calling_convention!="unknown"){
                wi(3); ws("\"calling_convention\": \""); ws(json_escape(s.calling_convention)); ws("\",\n");
            }
            if(!s.section.empty()){
                wi(3); ws("\"section\": \""); ws(json_escape(s.section)); ws("\",\n");
            }
            if(s.ranges.size()!=1 || s.ranges.empty() || s.ranges[0].rva != s.rva || s.ranges[0].size != s.size){
                wi(3); ws("\"ranges\": [");
                for(size_t ri=0;ri<s.ranges.size();++ri){ if(ri) ws(", "); ws("{\"rva\":"); ws(std::to_string(s.ranges[ri].rva)); ws(",\"size\":"); ws(std::to_string(s.ranges[ri].size)); ws("}"); }
                ws("],\n");
            }
            if(!s.instructions.empty()){
                wi(3); ws("\"instructions\": [");
                for(size_t ii=0;ii<s.instructions.size();++ii){ if(ii) ws(", "); ws("{\"offset\":"); ws(std::to_string(s.instructions[ii].offset)); ws(",\"size\":"); ws(std::to_string((int)s.instructions[ii].size)); ws(",\"is_call\":"); ws(s.instructions[ii].is_call?"true":"false"); ws(",\"is_ret\":"); ws(s.instructions[ii].is_ret?"true":"false"); ws("}"); }
                ws("],\n");
            }
            if(s.source){
                wi(3); ws("\"source\": {\"file\":\""); ws(json_escape(s.source->file)); ws("\",\"line\":"); ws(std::to_string(s.source->line)); ws("},\n");
            }
            wi(3); ws("\"typed_binding\": {\"available\":"); ws(s.typed_binding.available?"true":"false"); ws(",\"reason\":\""); ws(json_escape(s.typed_binding.reason)); ws("\"}\n");
            wi(2); ws("}");
            if(i+1<m.symbols.size()) ws(",");
            ws("\n");
        }
        wi(1); ws("],\n");
        wi(1); ws("\"stats\": {\n");
        wi(2); ws("\"total_symbols\": "); ws(std::to_string(m.stats.total_symbols)); ws(",\n");
        wi(2); ws("\"typed_available\": "); ws(std::to_string(m.stats.typed_available)); ws(",\n");
        wi(2); ws("\"raw_only\": "); ws(std::to_string(m.stats.raw_only)); ws(",\n");
        wi(2); ws("\"inlined_or_no_address\": "); ws(std::to_string(m.stats.inlined_or_no_address)); ws(",\n");
        wi(2); ws("\"functions\": "); ws(std::to_string(m.stats.functions)); ws(",\n");
        wi(2); ws("\"data\": "); ws(std::to_string(m.stats.data)); ws(",\n");
        wi(2); ws("\"vtables\": "); ws(std::to_string(m.stats.vtables)); ws(",\n");
        wi(2); ws("\"imports\": "); ws(std::to_string(m.stats.imports)); ws("\n");
        wi(1); ws("}\n");
        ws("}\n");
        if(!out) throw GenerationError("write failed: " + output_path);
    }
}

} // namespace
