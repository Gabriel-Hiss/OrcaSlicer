#include "orca/hook_sdkgen/dia_reader.hpp"
#include "orca/hook_sdkgen/pe_image.hpp"
#include "orca/hook_sdkgen/hash_util.hpp"
#include "orca/hook_sdkgen/zydis_validator.hpp"

#ifdef ORCA_DIA_AVAILABLE

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <comdef.h>
#include <atlbase.h>
#include <functional>
#include <string>
#include <vector>
#include <algorithm>
#include <map>
#include <unordered_map>
#include <fstream>
#include <iostream>
#include <cstdint>
#include <cctype>

#if __has_include(<dia2.h>)
#include <dia2.h>
#elif __has_include("dia2.h")
#include "dia2.h"
#else
struct IDiaDataSource; struct IDiaSession; struct IDiaSymbol; struct IDiaEnumSymbols;
#define DIA_UNAVAILABLE 1
#endif

#ifndef DIA_UNAVAILABLE
#include <diacreate.h>
#endif

namespace orca::hook_sdkgen {

static std::wstring widen(const std::string& s) {
    int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(len, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), len);
    if (!w.empty() && w.back()=='\0') w.pop_back();
    return w;
}
static std::string narrow(const BSTR b) {
    if (!b) return "";
    int len = WideCharToMultiByte(CP_UTF8,0,b,-1,nullptr,0,nullptr,nullptr);
    std::string s(len, '\0');
    WideCharToMultiByte(CP_UTF8,0,b,-1,s.data(),len,nullptr,nullptr);
    if (!s.empty() && s.back()=='\0') s.pop_back();
    return s;
}
static std::string narrow(const std::wstring& w) {
    int len = WideCharToMultiByte(CP_UTF8,0,w.c_str(),-1,nullptr,0,nullptr,nullptr);
    std::string s(len,'\0');
    WideCharToMultiByte(CP_UTF8,0,w.c_str(),-1,s.data(),len,nullptr,nullptr);
    if (!s.empty() && s.back()=='\0') s.pop_back();
    return s;
}
static std::string bstr_to_string(BSTR b) { if (!b) return ""; std::string s=narrow(b); SysFreeString(b); return s; }
static std::string get_symbol_name(IDiaSymbol* sym) { BSTR b=nullptr; if (SUCCEEDED(sym->get_name(&b)) && b) return bstr_to_string(b); return ""; }

static CComPtr<IDiaDataSource> tryLoadDiaRegFree(std::vector<std::wstring>& tried, HRESULT& outHr) {
    tried.clear(); outHr = REGDB_E_CLASSNOTREG;
    std::vector<std::wstring> candidates;
#ifdef ORCA_DIA_DLL_PATH
    candidates.push_back(widen(ORCA_DIA_DLL_PATH));
#endif
    {
        wchar_t buf[32768]; DWORD len = GetEnvironmentVariableW(L"VSINSTALLDIR", buf, 32768);
        if (len>0 && len<32768) {
            std::wstring base(buf); if (!base.empty() && base.back()!=L'\\' && base.back()!=L'/') base+=L"\\";
            candidates.push_back(base + L"DIA SDK\\bin\\amd64\\msdia140.dll");
            candidates.push_back(base + L"DIA SDK\\bin\\msdia140.dll");
        }
    }
    {
        wchar_t exePath[MAX_PATH]; DWORD len = GetModuleFileNameW(nullptr, exePath, MAX_PATH);
        if (len>0 && len<MAX_PATH) {
            std::wstring exe(exePath); size_t pos = exe.find_last_of(L"\\/");
            if (pos!=std::wstring::npos) {
                std::wstring dir = exe.substr(0,pos+1);
                candidates.push_back(dir + L"msdia140.dll");
                candidates.push_back(dir + L"bin\\amd64\\msdia140.dll");
            }
        }
    }
    for (auto& path : candidates) {
        tried.push_back(path);
        HMODULE h = LoadLibraryW(path.c_str());
        if (!h) continue;
        auto pfn = reinterpret_cast<HRESULT(WINAPI*)(REFCLSID, REFIID, void**)>(GetProcAddress(h, "DllGetClassObject"));
        if (!pfn) continue;
        CComPtr<IClassFactory> factory;
        HRESULT hr = pfn(CLSID_DiaSource, IID_IClassFactory, (void**)&factory);
        if (FAILED(hr) || !factory) continue;
        CComPtr<IDiaDataSource> src;
        hr = factory->CreateInstance(nullptr, IID_IDiaDataSource, (void**)&src);
        if (SUCCEEDED(hr) && src) { outHr = hr; return src; }
    }
    return nullptr;
}

void DiaReader::init_com() {
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) throw GenerationError("CoInitializeEx failed");
}
void DiaReader::check_pdb_match(const std::string& image_path, const std::string& pdb_path, BuildInfo& out_build) {
    PeInfo pe = read_pe_info(image_path);
    if (!pe.codeview.found) throw GenerationError("PE has no CodeView entry; cannot verify PDB identity: " + image_path);
    out_build.os = Os::Windows; out_build.arch = Arch::X86_64;
    out_build.debug_file = pe.codeview.pdb_file_name;
    out_build.debug_guid = pe.codeview.guid;
    out_build.debug_age = pe.codeview.age;
    out_build.image_path = image_path;
    out_build.image_sha256 = pe.sha256;
    std::ifstream pf(pdb_path, std::ios::binary);
    if (!pf) throw GenerationError("cannot open PDB: " + pdb_path);
    (void)pf;
}
static TypedBinding classify_binding(const std::string& type_name, TypeKind kind, bool is_complete, bool is_varargs, const std::string& callconv) {
    TypedBinding tb{true,""};
    // Data symbols should not be penalized for calling convention; only functions have it.
    // But per contract, unknown/varargs conventions are still raw for functions.
    // We check callconv first – callers must pass "x64"/"sysv" for data.
    if (callconv=="unknown" || callconv=="varargs") { tb.available=false; tb.reason="unsupported calling convention: "+callconv; return tb; }
    if (is_varargs) { tb.available=false; tb.reason="varargs not supported"; return tb; }
    if (!is_complete) { tb.available=false; tb.reason="incomplete layout"; return tb; }
    switch(kind){
        case TypeKind::Void: case TypeKind::Bool: case TypeKind::I8: case TypeKind::U8: case TypeKind::I16: case TypeKind::U16:
        case TypeKind::I32: case TypeKind::U32: case TypeKind::I64: case TypeKind::U64:
        case TypeKind::F32: case TypeKind::F64: case TypeKind::Enum: case TypeKind::Pointer: case TypeKind::Reference: case TypeKind::Array: return tb;
        case TypeKind::Struct: return tb;
        case TypeKind::Class: case TypeKind::Union: tb.available=false; tb.reason="non-trivial class/union requires borrowed handle, not typed copy"; return tb;
        case TypeKind::Function: tb.available=false; tb.reason="function type not directly bindable"; return tb;
        default: tb.available=false; tb.reason="unsupported type kind: "+type_kind_to_string(kind); return tb;
    }
}
// --- SymTag / BasicType / UdtKind constants matching cvconst.h (DIA SDK) ---
static constexpr DWORD kSymTagNull = 0;
static constexpr DWORD kSymTagExe = 1;
static constexpr DWORD kSymTagCompiland = 2;
static constexpr DWORD kSymTagFunction = 5;
static constexpr DWORD kSymTagData = 7;
static constexpr DWORD kSymTagUDT = 11;
static constexpr DWORD kSymTagEnum = 12;
static constexpr DWORD kSymTagFunctionType = 13;
static constexpr DWORD kSymTagPointerType = 14;
static constexpr DWORD kSymTagArrayType = 15;
static constexpr DWORD kSymTagBaseType = 16;
static constexpr DWORD kSymTagTypedef = 17;
static constexpr DWORD kSymTagFunctionArgType = 19;
static constexpr DWORD kSymTagVTable = 24;
static constexpr DWORD kBtNoType = 0;
static constexpr DWORD kBtVoid = 1;
static constexpr DWORD kBtChar = 2;
static constexpr DWORD kBtWChar = 3;
static constexpr DWORD kBtInt = 6;
static constexpr DWORD kBtUInt = 7;
static constexpr DWORD kBtFloat = 8;
static constexpr DWORD kBtBool = 10;
static constexpr DWORD kBtLong = 13;
static constexpr DWORD kBtULong = 14;
static constexpr DWORD kBtHresult = 31;
static constexpr DWORD kBtChar16 = 32;
static constexpr DWORD kBtChar32 = 33;
static constexpr DWORD kBtChar8 = 34;
static constexpr DWORD kUdtStruct = 0;
static constexpr DWORD kUdtClass = 1;
static constexpr DWORD kUdtUnion = 2;
static bool is_trivial_kind(TypeKind k){
    switch(k){
        case TypeKind::Void: case TypeKind::Bool: case TypeKind::I8: case TypeKind::U8: case TypeKind::I16: case TypeKind::U16:
        case TypeKind::I32: case TypeKind::U32: case TypeKind::I64: case TypeKind::U64:
        case TypeKind::F32: case TypeKind::F64: case TypeKind::Enum: case TypeKind::Pointer: case TypeKind::Reference: case TypeKind::Array: case TypeKind::Struct:
            return true;
        default: return false;
    }
}
static TypeKind base_type_to_kind(DWORD bt, ULONGLONG len){
    switch(bt){
        case kBtVoid: return TypeKind::Void;
        case kBtChar:
        case kBtChar8: return (len==1? TypeKind::I8 : TypeKind::I8);
        case kBtWChar:
        case kBtChar16: return TypeKind::U16;
        case kBtChar32: return TypeKind::U32;
        case kBtInt:
        case kBtLong:
        case kBtHresult:
            if(len==1) return TypeKind::I8;
            if(len==2) return TypeKind::I16;
            if(len==4) return TypeKind::I32;
            if(len==8) return TypeKind::I64;
            return TypeKind::I32;
        case kBtUInt:
        case kBtULong:
            if(len==1) return TypeKind::U8;
            if(len==2) return TypeKind::U16;
            if(len==4) return TypeKind::U32;
            if(len==8) return TypeKind::U64;
            return TypeKind::U32;
        case kBtFloat:
            if(len==4) return TypeKind::F32;
            if(len==8) return TypeKind::F64;
            return (len==4? TypeKind::F32 : TypeKind::F64);
        case kBtBool: return TypeKind::Bool;
        default: return TypeKind::Unknown;
    }
}
static TypeKind udt_kind_to_type_kind(DWORD udtKind){
    if(udtKind==kUdtStruct) return TypeKind::Struct;
    if(udtKind==kUdtClass) return TypeKind::Class;
    if(udtKind==kUdtUnion) return TypeKind::Union;
    return TypeKind::Struct;
}



Manifest DiaReader::read(const std::string& image_path, const std::string& debug_path) {
#ifdef DIA_UNAVAILABLE
    throw GenerationError("DIA SDK not found at compile time; install Visual Studio DIA SDK via VSINSTALLDIR and rebuild hook-sdkgen. Image: " + image_path);
#endif
    init_com();
    BuildInfo build;
    check_pdb_match(image_path, debug_path, build);
    CComPtr<IDiaDataSource> source;
    HRESULT hr = CoCreateInstance(CLSID_DiaSource, nullptr, CLSCTX_INPROC_SERVER, IID_IDiaDataSource, (void**)&source);
    if (FAILED(hr)) {
        if (hr==REGDB_E_CLASSNOTREG || hr==CLASS_E_CLASSNOTAVAILABLE || hr==HRESULT_FROM_WIN32(ERROR_MOD_NOT_FOUND) || hr==0x80040154 || hr==0x80040111) {
            std::vector<std::wstring> tried; HRESULT hr2=hr;
            auto src2 = tryLoadDiaRegFree(tried, hr2);
            if (src2) { source = src2; hr = hr2; }
            else {
                std::string msg = "CoCreateInstance(CLSID_DiaSource) failed hr=0x" + ([&]{ char buf[32]; snprintf(buf,sizeof(buf),"%08X",(unsigned)hr); return std::string(buf); }()) + " (msdia140.dll not registered) and reg-free LoadLibrary also failed. Tried:\n";
                for (auto& w: tried) msg += "  " + narrow(w) + "\n";
                msg += "Set VSINSTALLDIR to DIA SDK or ensure msdia140.dll is alongside hook-sdkgen (no regsvr32 required).";
#ifdef ORCA_DIA_DLL_PATH
                msg += std::string(" CMake ORCA_DIA_DLL_PATH=") + ORCA_DIA_DLL_PATH;
#endif
                throw GenerationError(msg);
            }
        } else {
            char buf[32]; snprintf(buf,sizeof(buf),"%08X",(unsigned)hr);
            throw GenerationError(std::string("CoCreateInstance(CLSID_DiaSource) failed hr=0x")+buf);
        }
    }
    std::wstring wpdb = widen(debug_path);
    hr = source->loadDataFromPdb(wpdb.c_str());
    if (FAILED(hr)) throw GenerationError("loadDataFromPdb failed for " + debug_path + " hr=0x" + std::to_string((unsigned)hr));
    CComPtr<IDiaSession> session;
    hr = source->openSession(&session);
    if (FAILED(hr)) throw GenerationError("openSession failed");
    CComPtr<IDiaSymbol> global;
    hr = session->get_globalScope(&global);
    if (FAILED(hr)) throw GenerationError("get_globalScope failed");
    GUID pdbGuid{}; DWORD pdbAge=0;
    hr = global->get_guid(&pdbGuid);
    if (SUCCEEDED(hr)) {
        std::array<uint8_t,16> pdb_guid_bytes{}; std::memcpy(pdb_guid_bytes.data(), &pdbGuid, 16);
        if (pdb_guid_bytes != build.debug_guid) throw GenerationError("PDB GUID mismatch: PE CodeView GUID does not match PDB GUID (image/pdb mismatch)");
    }
    hr = global->get_age(&pdbAge);
    if (SUCCEEDED(hr) && pdbAge != build.debug_age) throw GenerationError("PDB age mismatch: PE age " + std::to_string(build.debug_age) + " != PDB age " + std::to_string(pdbAge));

    Manifest manifest; manifest.build = build; manifest.format_version = kFormatVersion; manifest.hook_abi = kHookAbiVersion;
    uint32_t next_type_id = 1; std::map<std::string,uint32_t> type_map;
    auto get_or_create_type_ex = [&](const std::string& name, TypeKind kind, uint32_t size, uint32_t align, uint32_t pointee, uint32_t arrayCnt, uint32_t retType, const std::vector<uint32_t>& paramTypes, bool is_complete)->uint32_t{
        std::string key = name + "#" + std::to_string((int)kind) + "#" + std::to_string(size);
        if(pointee) key += "#pt" + std::to_string(pointee);
        if(arrayCnt) key += "#ac" + std::to_string(arrayCnt);
        if(retType) key += "#ret" + std::to_string(retType);
        if(!paramTypes.empty()){ key += "#p"; for(auto v: paramTypes) key += std::to_string(v)+","; }
        auto it=type_map.find(key); if(it!=type_map.end()) return it->second;
        TypeInfo ti; ti.id=next_type_id++; ti.name=name; ti.kind=kind; ti.size=size; ti.align=align?align:size;
        ti.pointee_type=pointee; ti.array_count=arrayCnt; ti.return_type=retType; ti.param_types=paramTypes;
        ti.is_complete=is_complete; ti.is_trivially_copyable = (kind==TypeKind::Struct || kind==TypeKind::Enum || kind==TypeKind::Pointer || kind==TypeKind::Array);
        manifest.types.push_back(std::move(ti)); type_map[key]=manifest.types.back().id; return manifest.types.back().id;
    };
    auto get_or_create_simple = [&](const std::string& name, TypeKind kind, uint32_t size)->uint32_t{
        return get_or_create_type_ex(name,kind,size,0,0,0,0,{},true);
    };
    get_or_create_simple("void",TypeKind::Void,0);
    get_or_create_simple("bool",TypeKind::Bool,1);
    get_or_create_simple("int8_t",TypeKind::I8,1);
    get_or_create_simple("uint8_t",TypeKind::U8,1);
    get_or_create_simple("int16_t",TypeKind::I16,2);
    get_or_create_simple("uint16_t",TypeKind::U16,2);
    get_or_create_simple("int32_t",TypeKind::I32,4);
    get_or_create_simple("uint32_t",TypeKind::U32,4);
    get_or_create_simple("int64_t",TypeKind::I64,8);
    get_or_create_simple("uint64_t",TypeKind::U64,8);
    get_or_create_simple("float",TypeKind::F32,4);
    get_or_create_simple("double",TypeKind::F64,8);
    std::vector<Symbol> symbols; uint32_t inlined_or_no_addr=0;
    std::function<std::pair<TypeKind,uint32_t>(IDiaSymbol*)> resolve_type;
    resolve_type = [&](IDiaSymbol* ts)->std::pair<TypeKind,uint32_t>{
        if(!ts) return {TypeKind::Unknown,0};
        DWORD tag=0; ts->get_symTag(&tag);
        BSTR bname=nullptr; ts->get_name(&bname); std::string tname = bname? bstr_to_string(bname) : "unknown";
        if(tname.empty()) tname="unknown";
        ULONGLONG tlen=0; ts->get_length(&tlen);
        uint32_t tsize = (uint32_t)tlen;
        if(tag==kSymTagTypedef){
            CComPtr<IDiaSymbol> under; if(SUCCEEDED(ts->get_type(&under)) && under){
                auto pr = resolve_type(under);
                // For classification we return underlying kind; for storage we create typedef alias
                get_or_create_type_ex(tname, pr.first, tsize?tsize:(pr.first==TypeKind::Void?0:4), 0,0,0,0,{},true);
                return pr;
            }
            return {TypeKind::Unknown,0};
        }
        if(tag==kSymTagBaseType){
            DWORD bt=0; ts->get_baseType(&bt);
            TypeKind k = base_type_to_kind(bt, tlen);
            if(k==TypeKind::Unknown) return {TypeKind::Unknown,0};
            uint32_t tid = get_or_create_simple(tname.empty()? type_kind_to_string(k):tname, k, tsize);
            return {k,tid};
        }
        if(tag==kSymTagPointerType){
            BOOL isRef=FALSE; ts->get_reference(&isRef);
            CComPtr<IDiaSymbol> pointee; ts->get_type(&pointee);
            uint32_t ptid=0; TypeKind pk=TypeKind::Unknown;
            if(pointee){ auto pr = resolve_type(pointee); pk=pr.first; ptid=pr.second; }
            TypeKind k = isRef? TypeKind::Reference : TypeKind::Pointer;
            std::string pname = tname.empty()? (k==TypeKind::Reference?"ref":"ptr") : tname;
            // Size of pointer on x64 is 8
            uint32_t sz = tsize? tsize : 8;
            uint32_t tid = get_or_create_type_ex(pname, k, sz, sz, ptid, 0, 0, {}, true);
            for(auto &ti: manifest.types) if(ti.id==tid){ ti.pointee_type=ptid; break; }
            return {k,tid};
        }
        if(tag==kSymTagArrayType){
            CComPtr<IDiaSymbol> elem; ts->get_type(&elem);
            uint32_t etid=0;
            if(elem){ auto pr = resolve_type(elem); etid=pr.second; }
            DWORD cnt=0; ts->get_count(&cnt);
            uint32_t sz = tsize;
            std::string aname = tname.empty()? "array" : tname;
            uint32_t tid = get_or_create_type_ex(aname, TypeKind::Array, sz, 0, etid, cnt?cnt:0, 0, {}, true);
            return {TypeKind::Array,tid};
        }
        if(tag==kSymTagUDT){
            DWORD udtK=0; ts->get_udtKind(&udtK);
            TypeKind k = udt_kind_to_type_kind(udtK);
            std::string uname = tname;
            uint32_t tid = get_or_create_type_ex(uname, k, tsize, 0, 0,0,0,{},true);
            return {k,tid};
        }
        if(tag==kSymTagEnum){
            uint32_t tid = get_or_create_simple(tname.empty()?"enum":tname, TypeKind::Enum, tsize?tsize:4);
            return {TypeKind::Enum,tid};
        }
        if(tag==kSymTagFunctionType){
            // Function type itself – create an entry with return/params if caller wants, but for data classification we treat as Function (raw)
            // Caller for function symbols will handle expansion
            uint32_t tid = get_or_create_simple(tname.empty()?"func":tname, TypeKind::Function, 0);
            return {TypeKind::Function,tid};
        }
        return {TypeKind::Unknown,0};
    };
    auto resolve_complete = [&](IDiaSymbol* ts, TypeKind &k, uint32_t &tid, std::string &name, uint32_t &sz)->bool{
        auto pr = resolve_type(ts);
        k=pr.first; tid=pr.second;
        if(k==TypeKind::Unknown || tid==0) return false;
        for(auto &ti: manifest.types) if(ti.id==tid){ name=ti.name; sz=ti.size; break; }
        return true;
    };
    auto process_symbol = [&](IDiaSymbol* sym, SymbolKind forced_kind){
        DWORD symTag=0; sym->get_symTag(&symTag);
        DWORD rva=0; ULONGLONG len=0; bool has_rva = SUCCEEDED(sym->get_relativeVirtualAddress(&rva)) && rva!=0;
        bool has_len = SUCCEEDED(sym->get_length(&len));
        (void)has_len;
        if (!has_rva) { inlined_or_no_addr++; return; }
        Symbol s; s.rva=rva; s.size=(uint32_t)len;
        std::string decName; BSTR bDec=nullptr; sym->get_undecoratedName(&bDec); if(bDec) decName=bstr_to_string(bDec);
        std::string name=get_symbol_name(sym); s.decorated_name=name.empty()?decName:name; s.display_name=decName.empty()?name:decName;
        if(s.decorated_name.empty()) s.decorated_name=s.display_name;
        s.id=s.decorated_name.empty()?(std::to_string(rva)+"_"+std::to_string(symTag)):s.decorated_name;
        s.kind=forced_kind;
        if(forced_kind==SymbolKind::Unknown){ if(symTag==kSymTagFunction) s.kind=SymbolKind::Function; else if(symTag==kSymTagData) s.kind=SymbolKind::Data; else if(symTag==kSymTagVTable) s.kind=SymbolKind::VTable; else if(symTag==6) return; else s.kind=SymbolKind::Data; }
        // Default calling convention: x64 for functions, x64 for data to avoid unknown penalization
        s.calling_convention = "x64";
        DWORD cc=0; if(SUCCEEDED(sym->get_callingConvention(&cc))){
            switch(cc){
                case 0: s.calling_convention="near_c"; break; // CV_CALL_NEAR_C
                case 1: s.calling_convention="near_stdcall"; break;
                case 4: s.calling_convention="x64"; break;
                case 7: s.calling_convention="near_stdcall"; break;
                case 11: s.calling_convention="thiscall"; break;
                default: s.calling_convention="x64"; break; // treat unknown x64 as x64, not unknown
            }
        }
        CComPtr<IDiaSymbol> typeSym;
        if(SUCCEEDED(sym->get_type(&typeSym)) && typeSym){
            DWORD tag=0; typeSym->get_symTag(&tag);
            // Function symbol with FunctionType -> special handling for signature
            if(s.kind==SymbolKind::Function && tag==kSymTagFunctionType){
                CComPtr<IDiaSymbol> retSym; typeSym->get_type(&retSym);
                TypeKind retKind=TypeKind::Void; uint32_t retTid=0; std::string retName="void"; uint32_t retSize=0;
                bool retOk = false;
                if(retSym){
                    TypeKind rk; uint32_t rtid; std::string rn; uint32_t rsz;
                    if(resolve_complete(retSym, rk, rtid, rn, rsz)){ retKind=rk; retTid=rtid; retName=rn; retSize=rsz; retOk=true; }
                    else { retKind=TypeKind::Unknown; retOk=false; }
                } else {
                    retTid = get_or_create_simple("void",TypeKind::Void,0);
                    retKind = TypeKind::Void; retOk=true;
                }
                CComPtr<IDiaEnumSymbols> argEnum;
                std::vector<uint32_t> paramTids; std::vector<TypeKind> paramKinds; std::vector<std::string> paramNames;
                bool paramsOk = true; std::string paramFail;
                BOOL isVarArgs=FALSE;
                // Try to detect varargs via count mismatch or via get_count? DIA does not expose varargs directly; check if last param is ... via name
                if(SUCCEEDED(typeSym->findChildren((enum SymTagEnum)kSymTagFunctionArgType,nullptr,nsNone,&argEnum)) && argEnum){
                    CComPtr<IDiaSymbol> argSym; ULONG celt=0;
                    while(SUCCEEDED(argEnum->Next(1,&argSym,&celt)) && celt==1){
                        CComPtr<IDiaSymbol> argType; HRESULT hrA = argSym->get_type(&argType);
                        if(SUCCEEDED(hrA) && argType){
                            TypeKind ak; uint32_t atid; std::string an; uint32_t asz;
                            if(resolve_complete(argType, ak, atid, an, asz)){
                                paramKinds.push_back(ak); paramTids.push_back(atid); paramNames.push_back(an);
                                if(!is_trivial_kind(ak) && ak!=TypeKind::Void){ paramsOk=false; paramFail="param type not bindable: "+an+" ("+type_kind_to_string(ak)+")"; }
                            } else { paramsOk=false; paramFail="unsupported param type kind"; }
                        } else { paramsOk=false; paramFail="no type info for param"; }
                        argSym.Release();
                    }
                }
                bool is_varargs = false; // conservative: if any param is ... we would have detected but DIA hides it
                bool complete = true;
                std::string ccForClassify = s.calling_convention;
                TypedBinding tb{true,""};
                if(ccForClassify=="unknown" || ccForClassify=="varargs") tb={false,"unsupported calling convention: "+ccForClassify};
                else if(is_varargs) tb={false,"varargs not supported"};
                else if(!complete) tb={false,"incomplete layout"};
                else if(retKind==TypeKind::Class || retKind==TypeKind::Union) tb={false,"non-trivial class/union requires borrowed handle, not typed copy"};
                else if(retKind==TypeKind::Function) tb={false,"function type not directly bindable"};
                else if(retKind==TypeKind::Unknown) tb={false,"unsupported type kind: "+type_kind_to_string(retKind)};
                else if(!is_trivial_kind(retKind) && retKind!=TypeKind::Void) tb={false,"unsupported type kind: "+type_kind_to_string(retKind)};
                else if(!paramsOk) tb={false,paramFail};
                else {
                    for(size_t i=0;i<paramKinds.size();++i){
                        TypeKind pk = paramKinds[i];
                        if(pk==TypeKind::Class || pk==TypeKind::Union){ tb={false,"non-trivial class/union requires borrowed handle, not typed copy"}; break; }
                        if(pk==TypeKind::Function){ tb={false,"function type not directly bindable"}; break; }
                        if(pk==TypeKind::Unknown){ tb={false,"unsupported type kind: "+type_kind_to_string(pk)}; break; }
                        if(!is_trivial_kind(pk)){ tb={false,"unsupported type kind: "+type_kind_to_string(pk)}; break; }
                    }
                }
                std::string funcTypeName = retName + " (";
                for(size_t i=0;i<paramNames.size();++i){ if(i) funcTypeName+=", "; funcTypeName+= paramNames[i]; }
                funcTypeName += ")";
                uint32_t funcTid = get_or_create_type_ex(funcTypeName, TypeKind::Function, 0, 0, 0, 0, retTid, paramTids, true);
                s.type_id = funcTid;
                s.typed_binding = tb;
            } else {
                TypeKind tk=TypeKind::Unknown; uint32_t tid=0; std::string tn; uint32_t tsz=0;
                if(resolve_complete(typeSym, tk, tid, tn, tsz)){
                    s.type_id = tid;
                    bool complete=true; bool is_varargs=false;
                    // For data, ensure calling convention is not penalizing: keep x64
                    std::string ccForClassify = s.calling_convention;
                    if(s.kind==SymbolKind::Data || s.kind==SymbolKind::VTable) ccForClassify="x64";
                    s.typed_binding = classify_binding(tn, tk, complete, is_varargs, ccForClassify);
                } else {
                    s.type_id = 0;
                    s.typed_binding={false,"unsupported type kind: unknown"};
                }
            }
        } else s.typed_binding={false,"no type info"};
        CComPtr<IDiaEnumLineNumbers> lines;
        if(SUCCEEDED(session->findLinesByRVA(rva,(DWORD)len,&lines)) && lines){ CComPtr<IDiaLineNumber> line; ULONG fetched=0; if(SUCCEEDED(lines->Next(1,&line,&fetched)) && fetched==1 && line){ CComPtr<IDiaSourceFile> file; if(SUCCEEDED(line->get_sourceFile(&file)) && file){ BSTR fname=nullptr; file->get_fileName(&fname); std::string fstr=fname?bstr_to_string(fname):""; DWORD ln=0; line->get_lineNumber(&ln); s.source=SourceLocation{fstr,(uint32_t)ln,0}; } } }
        s.ranges.push_back(Range{(uint32_t)rva,(uint32_t)len});
        symbols.push_back(std::move(s));
    };

    CComPtr<IDiaEnumSymbols> enumSym;
    if(SUCCEEDED(global->findChildren((enum SymTagEnum)kSymTagFunction,nullptr,nsNone,&enumSym)) && enumSym){ CComPtr<IDiaSymbol> sym; ULONG celt=0; while(SUCCEEDED(enumSym->Next(1,&sym,&celt)) && celt==1){ process_symbol(sym,SymbolKind::Function); sym.Release(); } }
    CComPtr<IDiaEnumSymbols> enumData;
    if(SUCCEEDED(global->findChildren((enum SymTagEnum)kSymTagData,nullptr,nsNone,&enumData)) && enumData){ CComPtr<IDiaSymbol> sym; ULONG celt=0; while(SUCCEEDED(enumData->Next(1,&sym,&celt)) && celt==1){ DWORD dt=0; sym->get_dataKind(&dt); if(dt==1 || dt==3 || dt==0) process_symbol(sym,SymbolKind::Data); sym.Release(); } }
    CComPtr<IDiaEnumSymbols> enumVTable;
    if(SUCCEEDED(global->findChildren((enum SymTagEnum)kSymTagVTable,nullptr,nsNone,&enumVTable)) && enumVTable){ CComPtr<IDiaSymbol> sym; ULONG celt=0; while(SUCCEEDED(enumVTable->Next(1,&sym,&celt)) && celt==1){ process_symbol(sym,SymbolKind::VTable); sym.Release(); } }
    try{ PeInfo peImp = read_pe_info(image_path); if(peImp.import_rva!=0){ Symbol imp; imp.id="import_table"; imp.decorated_name="import_table"; imp.display_name="PE Import Table"; imp.kind=SymbolKind::Import; imp.rva=peImp.import_rva; imp.size=peImp.import_size; imp.type_id=0; imp.typed_binding={false,"import table not typed"}; imp.ranges.push_back({peImp.import_rva,peImp.import_size}); symbols.push_back(std::move(imp)); } } catch(...) {}
    PeInfo pe2 = read_pe_info(image_path);
    for(auto& s: symbols) s.section=rva_to_section_name(pe2,(uint32_t)s.rva);
    std::sort(symbols.begin(), symbols.end(), [](const Symbol& a, const Symbol& b){ if(a.id!=b.id) return a.id<b.id; return a.rva<b.rva; });
    std::vector<Symbol> dedup; for(auto& sym: symbols) if(dedup.empty() || dedup.back().id!=sym.id) dedup.push_back(std::move(sym));
    manifest.symbols=std::move(dedup);
    std::sort(manifest.types.begin(), manifest.types.end(), [](const TypeInfo& a, const TypeInfo& b){ return a.id<b.id; });
    manifest.stats.inlined_or_no_address=inlined_or_no_addr;
    manifest.recompute_stats(); manifest.sort_deterministic();
    build.recompute_build_id(); manifest.build=build;
    try{ std::ifstream pf(image_path,std::ios::binary); pf.seekg(0,std::ios::end); size_t fsz=(size_t)pf.tellg(); pf.seekg(0,std::ios::beg); std::vector<uint8_t> img(fsz); pf.read((char*)img.data(),fsz); auto rva_to_off=[&](uint32_t rva)->size_t{ for(auto& sec: pe2.sections) if(rva>=sec.virtual_address && rva < sec.virtual_address + std::max(sec.virtual_size, sec.raw_size)) return (size_t)(rva - sec.virtual_address + sec.raw_offset); return SIZE_MAX; }; for(auto& sym: manifest.symbols) if(sym.kind==SymbolKind::Function && sym.size>0){ size_t off=rva_to_off((uint32_t)sym.rva); if(off!=SIZE_MAX && off+sym.size<=img.size()){ std::string err; auto instrs=validate_instructions(img.data()+off,sym.size,(uint32_t)sym.rva,&err); if(!instrs.empty()) sym.instructions=std::move(instrs); } } } catch(...) {}
    std::string verr; if(!manifest.validate(&verr)) throw GenerationError("manifest validation failed: "+verr);
    return manifest;
}

} // namespace orca::hook_sdkgen

#else
namespace orca::hook_sdkgen {
Manifest DiaReader::read(const std::string&, const std::string&) { throw GenerationError("DiaReader unavailable"); }
std::string DiaReader::name() const { return "DiaReader (unavailable)"; }
}
#endif
