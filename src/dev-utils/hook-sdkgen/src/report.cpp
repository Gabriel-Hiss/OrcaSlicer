#include "orca/hook_sdkgen/report.hpp"
#include "orca/hook_sdkgen/reader.hpp"
#include "orca/hook_sdkgen/zydis_validator.hpp"
#include <fstream>
#include <sstream>
namespace orca::hook_sdkgen {

static std::string json_escape(const std::string& s){
    std::string o; o.reserve(s.size());
    for(unsigned char c:s){ if(c=='\"') o+="\\\""; else if(c=='\\') o+="\\\\"; else if(c=='\n') o+="\\n"; else if(c=='\r') o+="\\r"; else if(c=='\t') o+="\\t"; else if(c<0x20){ char b[7]; std::snprintf(b,sizeof(b),"\\u%04x",c); o+=b; } else o+=(char)c; }
    return o;
}

GenerationReport make_report(const Manifest* manifest, const std::string& image_path, const std::string& pdb_path, const std::string& manifest_path, const std::string& runtime_path, const std::string& error){
    auto normalize_out = [](const std::string& p)->std::string{
        if(p.empty()) return p;
        std::string n=p;
        for(char &c: n) if(c=='\\') c='/';
        // Extract deterministic suffix starting at manifest/ or runtime/
        size_t pm = n.rfind("/manifest/");
        size_t pr = n.rfind("/runtime/");
        size_t pos = std::string::npos;
        if(pm!=std::string::npos) pos = pm+1;
        else if(pr!=std::string::npos) pos = pr+1;
        if(pos!=std::string::npos) return n.substr(pos);
        // Fallback: keep only filename prefixed with known folder when recognizable
        if(n.find("orca-hooks.json")!=std::string::npos) return std::string("manifest/") + n.substr(n.rfind('/')+1);
        if(n.find("orca-hooks.bin")!=std::string::npos) return std::string("runtime/") + n.substr(n.rfind('/')+1);
        if(n.find("hook-sdkgen-report.json")!=std::string::npos) return std::string("report/") + n.substr(n.rfind('/')+1);
        size_t sl = n.rfind('/');
        if(sl!=std::string::npos) return n.substr(sl+1);
        return n;
    };
    GenerationReport r;
    r.image_path=image_path; r.pdb_path=pdb_path; r.manifest_path=normalize_out(manifest_path); r.runtime_path=normalize_out(runtime_path);
    if (manifest){ r.build=manifest->build; r.stats=manifest->stats; r.success=error.empty(); } else { r.success=false; }
    r.error=error;
    auto zs=zydis_status(); r.zydis_status = zs.available? ("Zydis "+zs.version) : zs.reason;
#ifdef _WIN32
    r.dia_status = "DIA available (Windows)";
#else
    r.dia_status = "DIA unavailable (non-Windows; DiaReader excluded)";
#endif
    return r;
}

void write_report(const GenerationReport& report, const std::string& path){
    std::ostringstream oss;
    oss<<"{\n";
    oss<<"  \"image\": \""<<json_escape(report.image_path)<<"\",\n";
    oss<<"  \"pdb\": \""<<json_escape(report.pdb_path)<<"\",\n";
    oss<<"  \"manifest\": \""<<json_escape(report.manifest_path)<<"\",\n";
    oss<<"  \"runtime\": \""<<json_escape(report.runtime_path)<<"\",\n";
    oss<<"  \"success\": "<<(report.success?"true":"false")<<",\n";
    oss<<"  \"error\": \""<<json_escape(report.error)<<"\",\n";
    oss<<"  \"build\": {\n";
    oss<<"    \"os\": \""<<os_to_string(report.build.os)<<"\",\n";
    oss<<"    \"arch\": \""<<arch_to_string(report.build.arch)<<"\",\n";
    oss<<"    \"debug_guid\": \""<<report.build.debug_guid_string()<<"\",\n";
    oss<<"    \"debug_age\": "<<report.build.debug_age<<",\n";
    oss<<"    \"image_sha256\": \""<<report.build.image_sha256_hex()<<"\",\n";
    oss<<"    \"build_id\": \""<<json_escape(report.build.build_id)<<"\"\n";
    oss<<"  },\n";
    oss<<"  \"stats\": {\n";
    oss<<"    \"total_symbols\": "<<report.stats.total_symbols<<",\n";
    oss<<"    \"typed_available\": "<<report.stats.typed_available<<",\n";
    oss<<"    \"raw_only\": "<<report.stats.raw_only<<",\n";
    oss<<"    \"inlined_or_no_address\": "<<report.stats.inlined_or_no_address<<"\n";
    oss<<"  },\n";
    oss<<"  \"zydis\": \""<<json_escape(report.zydis_status)<<"\",\n";
    oss<<"  \"dia\": \""<<json_escape(report.dia_status)<<"\"\n";
    oss<<"}\n";
    std::ofstream out(path, std::ios::binary);
    if (!out) throw GenerationError("cannot write report: "+path);
    std::string s=oss.str();
    out.write(s.data(), s.size());
}

} // namespace
