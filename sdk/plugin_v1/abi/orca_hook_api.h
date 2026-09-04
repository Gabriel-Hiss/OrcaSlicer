#ifndef ORCA_HOOK_API_H
#define ORCA_HOOK_API_H

/*
 * OrcaSlicer Hook Plugin ABI v1
 * Public C ABI — sole source: sdk/plugin_v1/abi/orca_hook_api.h
 *
 * Constraints (plan §1.4, §3):
 *  - Fixed ORCA_HOOK_ABI_VERSION = 1
 *  - Structs are size-versioned (first uint32_t size, second uint32_t version/abi)
 *  - Fixed-width integers only, opaque handles, noexcept callbacks
 *  - No STL, no RTTI, no exceptions, no allocator crosses the boundary
 *  - Exports: orca_plugin_entry_v1 / orca_plugin_exit_v1
 *  - Host delivers orca_host_api_v1 table
 */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
#include <cstddef>
#endif

/* ------------------------------------------------------------------ */
/* Version                                                            */
/* ------------------------------------------------------------------ */

#define ORCA_HOOK_ABI_VERSION 1u

/* ------------------------------------------------------------------ */
/* Export / calling convention                                        */
/* ------------------------------------------------------------------ */

#if defined(_WIN32) || defined(__CYGWIN__)
#  if defined(ORCA_HOOK_BUILDING_PLUGIN)
#    define ORCA_HOOK_API __declspec(dllexport)
#  else
#    define ORCA_HOOK_API __declspec(dllimport)
#  endif
#else
#  if defined(__GNUC__) || defined(__clang__)
#    define ORCA_HOOK_API __attribute__((visibility("default")))
#  else
#    define ORCA_HOOK_API
#  endif
#endif

#ifdef __cplusplus
#  define ORCA_HOOK_NOEXCEPT noexcept
#else
#  define ORCA_HOOK_NOEXCEPT
#endif

/* ------------------------------------------------------------------ */
/* Status                                                             */
/* ------------------------------------------------------------------ */

typedef enum orca_hook_status {
    ORCA_HOOK_OK                       = 0,
    ORCA_HOOK_ERR_INVALID_ARG          = 1,
    ORCA_HOOK_ERR_INVALID_SIZE         = 2,
    ORCA_HOOK_ERR_UNSUPPORTED_ABI      = 3,
    ORCA_HOOK_ERR_BUILD_MISMATCH       = 4,
    ORCA_HOOK_ERR_NOT_FOUND            = 5,
    ORCA_HOOK_ERR_ALREADY_EXISTS       = 6,
    ORCA_HOOK_ERR_RESOLVE_FAILED       = 7,
    ORCA_HOOK_ERR_PATCH_FAILED         = 8,
    ORCA_HOOK_ERR_BAD_INSTRUCTION_BOUNDARY = 9,
    ORCA_HOOK_ERR_BAD_RVA              = 10,
    ORCA_HOOK_ERR_VTABLE_BOUNDS        = 11,
    ORCA_HOOK_ERR_IMPORT_NOT_FOUND     = 12,
    ORCA_HOOK_ERR_PROTECT_FAILED       = 13,
    ORCA_HOOK_ERR_BUSY                 = 14,
    ORCA_HOOK_ERR_RESTART_REQUIRED     = 15,
    ORCA_HOOK_ERR_JVM_UNAVAILABLE      = 16,
    ORCA_HOOK_ERR_INTERNAL             = 99
} orca_hook_status_t;

/* ------------------------------------------------------------------ */
/* Handles (opaque)                                                   */
/* ------------------------------------------------------------------ */

typedef void* orca_hook_handle_t;
typedef void* orca_plugin_handle_t; /* reserved, not used v1 — plugin is its own scope */

/* ------------------------------------------------------------------ */
/* Hook kinds and points                                              */
/* ------------------------------------------------------------------ */

typedef enum orca_hook_kind {
    ORCA_HOOK_KIND_BEFORE  = 0,
    ORCA_HOOK_KIND_AFTER   = 1,
    ORCA_HOOK_KIND_REPLACE = 2
} orca_hook_kind_t;

typedef enum orca_hook_point {
    ORCA_HOOK_POINT_ENTRY  = 0,  /* function entry (inline hook) */
    ORCA_HOOK_POINT_RETURN = 1,  /* function return(s) */
    ORCA_HOOK_POINT_INVOKE = 2,  /* call site inside target (requires ordinal) */
    ORCA_HOOK_POINT_OFFSET = 3,  /* arbitrary RVA inside function, at instruction boundary */
    ORCA_HOOK_POINT_VTABLE = 4,  /* vtable slot */
    ORCA_HOOK_POINT_IAT    = 5,  /* Windows IAT import hook */
    ORCA_HOOK_POINT_GOT    = 6   /* Linux GOT/PLT import hook — import_hook alias selects IAT/GOT per OS */
} orca_hook_point_t;

/* ------------------------------------------------------------------ */
/* Build id — exact match required by loader (plan §1.5)              */
/* Windows: GUID+age from CodeView + SHA256 of OrcaSlicer.dll         */
/* Linux:   GNU build-id + SHA256 of ELF                              */
/* ------------------------------------------------------------------ */

#define ORCA_BUILD_ID_OS_MAX   16
#define ORCA_BUILD_ID_ARCH_MAX 16

typedef struct orca_build_id {
    uint32_t size;        /* sizeof(orca_build_id) */
    uint32_t version;     /* ORCA_HOOK_ABI_VERSION */
    char     os[ORCA_BUILD_ID_OS_MAX];   /* "windows" | "linux" */
    char     arch[ORCA_BUILD_ID_ARCH_MAX]; /* "x86_64" */
    uint8_t  image_sha256[32];
    /* Windows CodeView identity — zeroed on Linux */
    uint8_t  pdb_guid[16];
    uint32_t pdb_age;
    uint32_t _pad0;
    /* Linux GNU build-id — zeroed on Windows */
    uint8_t  gnu_build_id[20];
    uint32_t gnu_build_id_size; /* 0..20 valid bytes */
    uint32_t _pad1;
} orca_build_id_t;

/* ------------------------------------------------------------------ */
/* Symbol handle — lightweight view over manifest symbol               */
/* ------------------------------------------------------------------ */

typedef struct orca_symbol_handle {
    uint32_t size;
    uint32_t version;
    const char* id;            /* manifest symbol id, e.g. "Slic3r::CLI::print_help" */
    const char* decorated_name;
    uint64_t   rva;
    uint64_t   size_bytes;
    uint32_t   type_id;
} orca_symbol_handle_t;

/* ------------------------------------------------------------------ */
/* x64 CPU context — raw hook primitive (plan §3.3, §3.4)             */
/* Saves/restores by assembly stubs per calling convention.            */
/* ------------------------------------------------------------------ */

typedef struct orca_xmm {
    uint64_t low;
    uint64_t high;
} orca_xmm_t;

typedef struct orca_cpu_context {
    uint32_t size;     /* sizeof(orca_cpu_context) */
    uint32_t version;  /* ORCA_HOOK_ABI_VERSION */
    uint64_t rax;
    uint64_t rbx;
    uint64_t rcx;
    uint64_t rdx;
    uint64_t rsi;
    uint64_t rdi;
    uint64_t rbp;
    uint64_t rsp;
    uint64_t r8;
    uint64_t r9;
    uint64_t r10;
    uint64_t r11;
    uint64_t r12;
    uint64_t r13;
    uint64_t r14;
    uint64_t r15;
    uint64_t rip;
    uint64_t rflags;
    orca_xmm_t xmm0;
    orca_xmm_t xmm1;
    orca_xmm_t xmm2;
    orca_xmm_t xmm3;
    orca_xmm_t xmm4;
    orca_xmm_t xmm5;
    orca_xmm_t xmm6;
    orca_xmm_t xmm7;
    orca_xmm_t xmm8;
    orca_xmm_t xmm9;
    orca_xmm_t xmm10;
    orca_xmm_t xmm11;
    orca_xmm_t xmm12;
    orca_xmm_t xmm13;
    orca_xmm_t xmm14;
    orca_xmm_t xmm15;
} orca_cpu_context_t;

/* Memory protection flags (platform-agnostic) */
typedef enum orca_protect {
    ORCA_PROTECT_READ        = 1u << 0,
    ORCA_PROTECT_WRITE       = 1u << 1,
    ORCA_PROTECT_EXECUTE     = 1u << 2
} orca_protect_t;

/* ------------------------------------------------------------------ */
/* Hook request — size-versioned, describes one hook installation     */
/* Chain: priority default 1000, larger runs first. Tie: plugin id    */
/* then declaration order. before high→low, replace wraps next (call  */
/* at most once), original ends chain, after low→high (plan §3.2).    */
/* ------------------------------------------------------------------ */

typedef struct orca_hook_request {
    uint32_t size;                 /* sizeof(orca_hook_request) */
    uint32_t version;              /* ORCA_HOOK_ABI_VERSION */

    const char* hook_id;           /* unique within plugin, e.g. "on_print_help" */
    const char* target_symbol_id;  /* manifest symbol id — preferred */
    uint64_t    target_rva;        /* alternative when id is NULL — RVA of target function */

    orca_hook_point_t point;
    orca_hook_kind_t  kind;
    uint32_t    priority;          /* default 1000 */

    /* Point-specific payload — only field matching point is read */
    union {
        struct { uint64_t rva; } offset; /* ORCA_HOOK_POINT_OFFSET — RVA must be instruction boundary */
        struct { uint32_t index; uint32_t is_per_instance; void* instance; } vtable; /* VTABLE */
        struct { const char* module; const char* name; } import_; /* IAT/GOT — import_hook */
        struct { uint32_t ordinal; } invoke; /* INVOKE — nth call inside target */
    } u;

    /* Callback — see orca_hook_callback_t / orca_replace_callback_t */
    void*     callback;
    void*     user_data;
} orca_hook_request_t;

/* ------------------------------------------------------------------ */
/* Callback signatures and result                                     */
/* before/after: may mutate CpuContext (args/return). out_result      */
/* is the unambiguous action channel: set action to SKIP_ORIGINAL to  */
/* suppress original (before), or CONTINUE to proceed. AFTER may      */
/* mutate ctx.rax/xmm return value even when before cancelled.        */
/* Replace receives next trampoline, may call it at most once.        */
/* Reentrancy: same hook on same thread skips its own record only.    */
/* Loader scopes synchronous plugin entry via thread-local storage;    */
/* no plugin_id field is needed in the request — host knows caller    */
/* from TLS during orca_plugin_entry_v1 and install_hook.             */
/* ------------------------------------------------------------------ */

typedef enum orca_hook_action {
    ORCA_HOOK_ACTION_CONTINUE      = 0, /* proceed to next/original */
    ORCA_HOOK_ACTION_SKIP_ORIGINAL = 1  /* before: skip original, after may still run; return value in ctx */
} orca_hook_action_t;

typedef struct orca_hook_result {
    uint32_t size;
    uint32_t version;
    orca_hook_action_t action;
    uint32_t _pad;
} orca_hook_result_t;

/* before / after / mid / vtable / import — out_result is required;   */
/* callee must initialize with size/version and set action. Caller     */
/* provides storage; callback returns status for error reporting,      */
/* not for chain control (use out_result->action for SKIP/CONTINUE).   */
typedef orca_hook_status_t (*orca_hook_callback_t)(
    orca_cpu_context_t* ctx,
    orca_hook_result_t* out_result,
    void*               user_data) ORCA_HOOK_NOEXCEPT;

/* replace — receives next dispatcher, may call it at most once via    */
/* next(ctx). out_result controls skip/continue as above.              */
typedef orca_hook_status_t (*orca_replace_callback_t)(
    orca_cpu_context_t* ctx,
    orca_hook_result_t* out_result,
    orca_hook_status_t (*next)(orca_cpu_context_t* ctx) ORCA_HOOK_NOEXCEPT,
    void*               user_data) ORCA_HOOK_NOEXCEPT;

/* ------------------------------------------------------------------ */
/* Host function table delivered to orca_plugin_entry_v1               */
/* Lifecycle: host owns table for process lifetime; plugin must not   */
/* retain stale pointers after orca_plugin_exit_v1.                    */
/* ------------------------------------------------------------------ */

typedef struct orca_host_api_v1 {
    uint32_t size;     /* sizeof(orca_host_api_v1) */
    uint32_t version;  /* ORCA_HOOK_ABI_VERSION */

    /* Build / symbol resolution */
    orca_hook_status_t (*get_build_id)(orca_build_id_t* out) ORCA_HOOK_NOEXCEPT;
    orca_hook_status_t (*resolve_symbol)(
        const char* symbol_id,
        void**      out_address,
        uint64_t*   out_rva) ORCA_HOOK_NOEXCEPT;
    orca_hook_status_t (*resolve_rva)(
        uint64_t rva,
        void**   out_address) ORCA_HOOK_NOEXCEPT;

    /* Hook registry — transaction: batch install fails atomically */
    orca_hook_status_t (*install_hook)(
        const orca_hook_request_t* request,
        orca_hook_handle_t*        out_handle) ORCA_HOOK_NOEXCEPT;
    orca_hook_status_t (*remove_hook)(
        orca_hook_handle_t handle) ORCA_HOOK_NOEXCEPT;

    /* Chain dispatch — only valid inside replace callback */
    orca_hook_status_t (*call_next)(
        orca_cpu_context_t* ctx) ORCA_HOOK_NOEXCEPT;
    orca_hook_status_t (*call_original)(
        orca_cpu_context_t* ctx) ORCA_HOOK_NOEXCEPT;

    /* Raw memory primitives — explicit size, temp protect + icache flush */
    orca_hook_status_t (*read_memory)(
        const void* src,
        void*       dst,
        size_t      size) ORCA_HOOK_NOEXCEPT;
    orca_hook_status_t (*write_memory)(
        void*       dst,
        const void* src,
        size_t      size) ORCA_HOOK_NOEXCEPT;
    orca_hook_status_t (*protect_memory)(
        void*    address,
        size_t   size,
        uint32_t new_protect, /* bitmask of orca_protect_t */
        uint32_t* out_old_protect) ORCA_HOOK_NOEXCEPT;
    orca_hook_status_t (*flush_icache)(
        void*  address,
        size_t size) ORCA_HOOK_NOEXCEPT;

    /* Diagnostics — thread-safe, survives hook disable */
    void (*log)(
        int32_t     level, /* 0=info 1=warn 2=error */
        const char* message) ORCA_HOOK_NOEXCEPT;
    orca_hook_status_t (*set_error)(
        const char* message) ORCA_HOOK_NOEXCEPT;

    /* Data dir — returns absolute data dir (host's --datadir or default) */
    const char* (*get_data_dir)(void) ORCA_HOOK_NOEXCEPT;

    /* Reserved for forward compat — must be NULL in v1 */
    void* _reserved[7];
} orca_host_api_v1_t;

/* Alias without _t suffix for spec text */
typedef struct orca_host_api_v1 orca_host_api_v1;

/* ------------------------------------------------------------------ */
/* Plugin entry / exit — only two exported symbols (C ABI)            */
/* ------------------------------------------------------------------ */

#ifdef __cplusplus
extern "C" {
#endif

ORCA_HOOK_API orca_hook_status_t orca_plugin_entry_v1(
    const orca_host_api_v1_t* host) ORCA_HOOK_NOEXCEPT;

ORCA_HOOK_API orca_hook_status_t orca_plugin_exit_v1(
    void) ORCA_HOOK_NOEXCEPT;

#ifdef __cplusplus
} /* extern "C" */
#endif

/* ------------------------------------------------------------------ */
/* C++ helpers — noexcept, header-only, never cross the C boundary    */
/* ------------------------------------------------------------------ */

#ifdef __cplusplus

namespace orca_hook {

inline orca_build_id_t make_build_id() noexcept {
    orca_build_id_t id{};
    id.size    = static_cast<uint32_t>(sizeof(orca_build_id_t));
    id.version = ORCA_HOOK_ABI_VERSION;
    return id;
}

inline orca_cpu_context_t make_cpu_context() noexcept {
    orca_cpu_context_t ctx{};
    ctx.size    = static_cast<uint32_t>(sizeof(orca_cpu_context_t));
    ctx.version = ORCA_HOOK_ABI_VERSION;
    return ctx;
}

inline orca_hook_request_t make_hook_request() noexcept {
    orca_hook_request_t r{};
    r.size    = static_cast<uint32_t>(sizeof(orca_hook_request_t));
    r.version = ORCA_HOOK_ABI_VERSION;
    r.priority = 1000u;
    r.point    = ORCA_HOOK_POINT_ENTRY;
    r.kind     = ORCA_HOOK_KIND_BEFORE;
    return r;
}

inline orca_host_api_v1_t make_host_api() noexcept {
    orca_host_api_v1_t h{};
    h.size    = static_cast<uint32_t>(sizeof(orca_host_api_v1_t));
    h.version = ORCA_HOOK_ABI_VERSION;
    return h;
}

inline bool host_api_valid(const orca_host_api_v1_t* h) noexcept {
    return h != nullptr
        && h->size >= sizeof(orca_host_api_v1_t)
        && h->version == ORCA_HOOK_ABI_VERSION
        && h->resolve_symbol != nullptr
        && h->install_hook != nullptr
        && h->remove_hook != nullptr;
}

inline const char* hook_status_string(orca_hook_status_t s) noexcept {
    switch (s) {
        case ORCA_HOOK_OK: return "OK";
        case ORCA_HOOK_ERR_INVALID_ARG: return "INVALID_ARG";
        case ORCA_HOOK_ERR_INVALID_SIZE: return "INVALID_SIZE";
        case ORCA_HOOK_ERR_UNSUPPORTED_ABI: return "UNSUPPORTED_ABI";
        case ORCA_HOOK_ERR_BUILD_MISMATCH: return "BUILD_MISMATCH";
        case ORCA_HOOK_ERR_NOT_FOUND: return "NOT_FOUND";
        case ORCA_HOOK_ERR_ALREADY_EXISTS: return "ALREADY_EXISTS";
        case ORCA_HOOK_ERR_RESOLVE_FAILED: return "RESOLVE_FAILED";
        case ORCA_HOOK_ERR_PATCH_FAILED: return "PATCH_FAILED";
        case ORCA_HOOK_ERR_BAD_INSTRUCTION_BOUNDARY: return "BAD_INSTRUCTION_BOUNDARY";
        case ORCA_HOOK_ERR_BAD_RVA: return "BAD_RVA";
        case ORCA_HOOK_ERR_VTABLE_BOUNDS: return "VTABLE_BOUNDS";
        case ORCA_HOOK_ERR_IMPORT_NOT_FOUND: return "IMPORT_NOT_FOUND";
        case ORCA_HOOK_ERR_PROTECT_FAILED: return "PROTECT_FAILED";
        case ORCA_HOOK_ERR_BUSY: return "BUSY";
        case ORCA_HOOK_ERR_RESTART_REQUIRED: return "RESTART_REQUIRED";
        case ORCA_HOOK_ERR_JVM_UNAVAILABLE: return "JVM_UNAVAILABLE";
        case ORCA_HOOK_ERR_INTERNAL: return "INTERNAL";
        default: return "UNKNOWN";
    }
}

} /* namespace orca_hook */
#endif /* __cplusplus */

#endif /* ORCA_HOOK_API_H */
