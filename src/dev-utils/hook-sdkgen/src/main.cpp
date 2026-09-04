#include "orca/hook_sdkgen/model.hpp"
#include "orca/hook_sdkgen/reader.hpp"
#include "orca/hook_sdkgen/emitter.hpp"
#include "orca/hook_sdkgen/manifest_emitter.hpp"
#include "orca/hook_sdkgen/binary_emitter.hpp"
#include "orca/hook_sdkgen/report.hpp"
#include "orca/hook_sdkgen/pe_image.hpp"
#include "orca/hook_sdkgen/hash_util.hpp"
#include "orca/hook_sdkgen/zydis_validator.hpp"
#include "orca/hook_sdkgen/sdk_emitter.hpp"
#include "orca/hook_sdkgen/dia_reader.hpp"
#include "orca/hook_sdkgen/dwarf_reader.hpp"
#include <string>
#include <iostream>
#include <filesystem>
#include <vector>
#include <fstream>
#include <map>
#include <algorithm>
#ifdef HAVE_ZLIB
#include <zlib.h>
#endif
using namespace orca::hook_sdkgen;
#ifdef HAVE_ZLIB
static bool compress_file_gz(const std::string& src_path, const std::string& dst_path) {
    std::ifstream in(src_path, std::ios::binary);
    if (!in) return false;
    std::vector<uint8_t> src((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    uLong src_len = (uLong)src.size();
    // Use raw deflate for gzip body
    z_stream strm{}; strm.zalloc=Z_NULL; strm.zfree=Z_NULL; strm.opaque=Z_NULL;
    if (deflateInit2(&strm, Z_BEST_COMPRESSION, Z_DEFLATED, -15, 8, Z_DEFAULT_STRATEGY)!=Z_OK) return false;
    uLong bound = compressBound(src_len) + 18;
    std::vector<uint8_t> raw(bound);
    strm.next_in = src.data(); strm.avail_in = (uInt)src_len;
    strm.next_out = raw.data(); strm.avail_out = (uInt)bound;
    int ret = deflate(&strm, Z_FINISH);
    if (ret != Z_STREAM_END) { deflateEnd(&strm); return false; }
    size_t raw_len = strm.total_out;
    deflateEnd(&strm);
    std::ofstream out(dst_path, std::ios::binary);
    if (!out) return false;
    uint8_t gz_hdr[10] = {0x1f,0x8b,0x08,0x00,0x00,0x00,0x00,0x00,0x00,0xff};
    out.write(reinterpret_cast<char*>(gz_hdr), 10);
    out.write(reinterpret_cast<char*>(raw.data()), raw_len);
    uint32_t crc = crc32(0L, Z_NULL, 0);
    crc = crc32(crc, src.data(), src_len);
    uint32_t isize = (uint32_t)src_len;
    uint8_t footer[8]; footer[0]=crc&0xFF; footer[1]=(crc>>8)&0xFF; footer[2]=(crc>>16)&0xFF; footer[3]=(crc>>24)&0xFF;
    footer[4]=isize&0xFF; footer[5]=(isize>>8)&0xFF; footer[6]=(isize>>16)&0xFF; footer[7]=(isize>>24)&0xFF;
    out.write(reinterpret_cast<char*>(footer),8);
    return (bool)out;
}
#endif
static void print_usage(const char* prog){
    std::cout<<"Usage: "<<prog<<" --pe <OrcaSlicer.dll> --pdb <OrcaSlicer.pdb> --out <dir> [options]\n"
               "       "<<prog<<" --elf <orca-slicer> --out <dir> [options]  (Linux)\n"
               "Options:\n"
               "  --pe <path>        PE image path (Windows) or ELF path (Linux via --elf alias)\n"
               "  --elf <path>       ELF image path (Linux, alias for --pe)\n"
               "  --pdb <path>       PDB path (Windows) or debug file (Linux, optional)\n"
               "  --out <dir>        Output directory (will contain manifest/orca-hooks.json, runtime/orca-hooks.bin, report.json, and plugin-sdk/<build-id>)\n"
               "  --manifest <path>  Override manifest output path\n"
               "  --runtime <path>   Override runtime binary output path\n"
               "  --report <path>    Override report output path\n"
               "  --no-sdk           Skip SDK generation (SDKs emitted by default)\n"
               "  --help             Show help\n";
}
int main(int argc, char* argv[]){
    std::string pe, pdb, out_dir, manifest_path, runtime_path, report_path;
    bool skip_sdk = false;
    for(int i=1;i<argc;++i){
        std::string a=argv[i];
        auto need=[&](std::string& out, const char* name){ if(i+1>=argc){ std::cerr<<"missing value for "<<name<<"\n"; return false; } out=argv[++i]; return true; };
        if(a=="--pe") { if(!need(pe,"--pe")) return 2; }
        else if(a=="--elf") { if(!need(pe,"--elf")) return 2; }
        else if(a=="--pdb") { if(!need(pdb,"--pdb")) return 2; }
        else if(a=="--out") { if(!need(out_dir,"--out")) return 2; }
        else if(a=="--manifest") { if(!need(manifest_path,"--manifest")) return 2; }
        else if(a=="--runtime") { if(!need(runtime_path,"--runtime")) return 2; }
        else if(a=="--report") { if(!need(report_path,"--report")) return 2; }
        else if(a=="--no-sdk") { skip_sdk=true; }
        else if(a=="--help" || a=="-h"){ print_usage(argv[0]); return 0; }
        else { std::cerr<<"unknown arg: "<<a<<"\n"; print_usage(argv[0]); return 2; }
    }
    if (pe.empty()){ std::cerr<<"--pe/--elf required\n"; print_usage(argv[0]); return 2; }
    if (out_dir.empty() && manifest_path.empty()){ std::cerr<<"--out or --manifest required\n"; print_usage(argv[0]); return 2; }
    namespace fs = std::filesystem;
    if (manifest_path.empty() && !out_dir.empty()) {
#ifdef HAVE_ZLIB
        manifest_path = (fs::path(out_dir)/"manifest"/"orca-hooks.json.gz").string();
#else
        manifest_path = (fs::path(out_dir)/"manifest"/"orca-hooks.json").string();
#endif
    }
    if (runtime_path.empty() && !out_dir.empty()) runtime_path = (fs::path(out_dir)/"runtime"/"orca-hooks.bin").string();
    if (report_path.empty() && !out_dir.empty()) report_path = (fs::path(out_dir)/"report"/"hook-sdkgen-report.json").string();
    try{
        if (!manifest_path.empty()) fs::create_directories(fs::path(manifest_path).parent_path());
        if (!runtime_path.empty()) fs::create_directories(fs::path(runtime_path).parent_path());
        if (!report_path.empty()) fs::create_directories(fs::path(report_path).parent_path());
    } catch (std::exception& e){ std::cerr<<"failed to create output directories: "<<e.what()<<"\n"; return 1; }
#ifdef _WIN32
    if (pdb.empty()){
        std::cerr<<"--pdb required on Windows\n";
        return 2;
    }
#else
    (void)pdb;
#endif
    std::string error;
    Manifest manifest;
    bool success=false;
    try{
#ifdef _WIN32
        auto reader = create_dia_reader();
        manifest = reader->read(pe, pdb);
#else
        // Linux x64: ELF/DWARF via libdwarf.
        // Requires ELF built with -g not stripped; libdwarf 2.3.2 SHA 7992e7b9019ebfabdda5773e86243517c48cf89fafed3209e853692bc9573efd.
        // If pdb is non-empty it is the separate debug file; otherwise pe is used for both.
        {
            auto reader = create_dwarf_reader();
            std::string image = pe;
            std::string debug = pdb.empty() ? pe : pdb;
            manifest = reader->read(image, debug);
        }
#endif
        manifest.recompute_stats();
        std::string verr;
        if (!manifest.validate(&verr)) throw GenerationError("manifest validation failed: "+verr);
        auto zs = zydis_status();
        if (!zs.available) {
            std::cerr<<"warning: Zydis unavailable: "<<zs.reason<<" — continuing without instruction validation\n";
        }
        if (!manifest_path.empty()){
            ManifestEmitter me; me.emit(manifest, manifest_path);
            std::cout<<"manifest written: "<<manifest_path<<" ("<<fs::file_size(manifest_path)<<" bytes)\n";
        }
        if (!runtime_path.empty()){
            BinaryEmitter be; be.emit(manifest, runtime_path);
            std::cout<<"runtime written: "<<runtime_path<<" ("<<fs::file_size(runtime_path)<<" bytes)\n";
            std::string berr;
            if (!BinaryEmitter::validate_file(runtime_path,&berr)) throw GenerationError("binary validation failed: "+berr);
        }
        if (!skip_sdk && !out_dir.empty()){
            emit_sdks(manifest, out_dir);
            std::cout<<"sdks written under "<<out_dir<<"/plugin-sdk/"<<manifest.build.build_id<<"\n";
        }
        success=true;
    } catch (const GenerationError& e){
        error = e.what();
        std::cerr<<"generation failed: "<<error<<"\n";
    } catch (const std::exception& e){
        error = e.what();
        std::cerr<<"unexpected error: "<<error<<"\n";
    }
    if (!report_path.empty()){
        try{
            GenerationReport rep = make_report(success? &manifest : nullptr, pe, pdb, manifest_path, runtime_path, error);
            write_report(rep, report_path);
            std::cout<<"report written: "<<report_path<<"\n";
        } catch (std::exception& e){
            std::cerr<<"failed to write report: "<<e.what()<<"\n";
        }
    }
    if (!success) {
        return 1;
    }
    {
        size_t with_type = 0, without_type = 0, with_signature = 0;
        std::map<std::string, size_t> reason_hist;
        size_t typed = 0, raw = 0;
        for (auto &s : manifest.symbols) {
            if (s.type_id != 0) with_type++; else without_type++;
            if (s.typed_binding.available) typed++; else { raw++; reason_hist[s.typed_binding.reason]++; }
            if (s.type_id != 0) with_signature++;
        }
        std::cout<<"instrumentation: symbols_with_type="<<with_type<<" without_type="<<without_type<<" with_signature="<<with_signature<<" types_table="<<manifest.types.size()<<"\n";
        std::cout<<"distribution raw_reasons (top):\n";
        std::vector<std::pair<std::string,size_t>> sorted(reason_hist.begin(), reason_hist.end());
        std::sort(sorted.begin(), sorted.end(), [](auto &a, auto &b){return a.second > b.second;});
        size_t show = std::min<size_t>(10, sorted.size());
        for (size_t i=0;i<show;++i) {
            std::cout<<"  ["<<sorted[i].second<<"] "<<sorted[i].first<<"\n";
        }
        if (sorted.empty()) std::cout<<"  (none – all typed)\n";
        std::string dominant = sorted.empty()? "(none)" : sorted.front().first;
        std::cout<<"dominant_raw_reason: "<<dominant<<" ("<<(sorted.empty()?0:sorted.front().second)<<")\n";
        std::cout<<"build_id: "<<manifest.build.build_id<<"  symbols: "<<manifest.stats.total_symbols<<" typed: "<<manifest.stats.typed_available<<" raw: "<<manifest.stats.raw_only<<" inlined/no_addr: "<<manifest.stats.inlined_or_no_address<<" types: "<<manifest.types.size()<<"\n";
    }
    return 0;

}
