use std::ffi::c_void;
use std::sync::atomic::{AtomicBool, AtomicPtr, Ordering};

static PANIC_DISABLED: AtomicBool = AtomicBool::new(false);
static G_HOST: AtomicPtr<OrcaHostApiV1> = AtomicPtr::new(std::ptr::null_mut());
#[repr(C)] pub struct OrcaBuildId { _priv: [u8;0] }
#[repr(C)] pub struct OrcaHostApiV1 { pub size: u32, pub version: u32, pub get_build_id: Option<extern "C" fn(*mut OrcaBuildId) -> i32>, pub resolve_symbol: Option<extern "C" fn(*const i8, *mut *mut c_void, *mut u64) -> i32>, pub resolve_rva: Option<extern "C" fn(u64, *mut *mut c_void) -> i32>, pub install_hook: Option<extern "C" fn(*const OrcaHookRequest, *mut *mut c_void) -> i32>, pub remove_hook: Option<extern "C" fn(*mut c_void) -> i32>, pub call_next: Option<extern "C" fn(*mut OrcaCpuContext) -> i32>, pub call_original: Option<extern "C" fn(*mut OrcaCpuContext) -> i32>, pub read_memory: Option<extern "C" fn(*const c_void, *mut c_void, usize) -> i32>, pub write_memory: Option<extern "C" fn(*mut c_void, *const c_void, usize) -> i32>, pub protect_memory: Option<extern "C" fn(*mut c_void, usize, u32, *mut u32) -> i32>, pub flush_icache: Option<extern "C" fn(*mut c_void, usize) -> i32>, pub log: Option<extern "C" fn(i32, *const i8)>, pub set_error: Option<extern "C" fn(*const i8) -> i32>, pub _reserved: [*mut c_void; 8], }
#[repr(C)] pub struct OrcaHookRequest { pub size: u32, pub version: u32, pub hook_id: *const i8, pub target_symbol_id: *const i8, pub target_rva: u64, pub point: u32, pub kind: u32, pub priority: u32, pub u_offset_rva: u64, pub _u_pad: u32, pub _u_pad2: u32, pub callback: *mut c_void, pub user_data: *mut c_void, }
#[repr(C)] pub struct OrcaCpuContext { pub size: u32, pub _pad: u32, pub rax: u64, pub rbx: u64, pub rcx: u64, pub rdx: u64, pub rsi: u64, pub rdi: u64, pub rbp: u64, pub rsp: u64, pub r8: u64, pub r9: u64, pub r10: u64, pub r11: u64, pub r12: u64, pub r13: u64, pub r14: u64, pub r15: u64, pub rip: u64, pub rflags: u64, pub xmm: [[u8; 16]; 16], }
#[repr(C)] pub struct OrcaHookResult { pub size: u32, pub version: u32, pub action: u32, pub _pad: u32 }
fn data_dir() -> std::path::PathBuf { if let Ok(d) = std::env::var("ORCA_DATA_DIR") { std::path::PathBuf::from(d) } else if let Ok(a) = std::env::var("APPDATA") { std::path::PathBuf::from(a).join("OrcaSlicer") } else { std::path::PathBuf::from(".") } }
fn log_path() -> std::path::PathBuf { data_dir().join("orca_plugins").join("demo-logs").join("rust-demo.log") }
fn append_log_line(line: &str) { let p = log_path(); let _ = std::fs::create_dir_all(p.parent().unwrap()); use std::io::Write; let mut f = std::fs::OpenOptions::new().create(true).append(true).open(&p).unwrap_or_else(|_| std::fs::File::create(&p).unwrap()); let mut s = line.to_string(); if !s.ends_with('\n') { s.push('\n'); } let _ = f.write_all(s.as_bytes()); }
fn hook_status_str(code: i32) -> &'static str { match code { 0=>"OK",1=>"INVALID_ARG",2=>"INVALID_SIZE",3=>"UNSUPPORTED_ABI",4=>"BUILD_MISMATCH",5=>"NOT_FOUND",6=>"ALREADY_EXISTS",7=>"RESOLVE_FAILED",8=>"PATCH_FAILED",9=>"BAD_INSTRUCTION_BOUNDARY",10=>"BAD_RVA",11=>"VTABLE_BOUNDS",12=>"IMPORT_NOT_FOUND",13=>"PROTECT_FAILED",14=>"BUSY",15=>"RESTART_REQUIRED",16=>"JVM_UNAVAILABLE",99=>"INTERNAL",_=>"UNKNOWN"} }
extern "C" fn before_get_time_tramp(ctx: *mut OrcaCpuContext, out: *mut OrcaHookResult, _ud: *mut c_void) -> i32 {
    let res = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
        if ctx.is_null() { append_log_line("rust-demo before Slic3r::Utils::get_current_time_utc ctx=null"); return; }
        let c = unsafe { &*ctx };
        let msg = format!("rust-demo before Slic3r::Utils::get_current_time_utc rip=0x{:x} rsp=0x{:x} rax=0x{:x} rcx=0x{:x} rdx=0x{:x} r8=0x{:x} size={} version={}", c.rip, c.rsp, c.rax, c.rcx, c.rdx, c.r8, c.size, c._pad);
        append_log_line(&msg);
    }));
    if res.is_err() { append_log_line("rust-demo panic_contained hook=before_get_time"); PANIC_DISABLED.store(true, Ordering::SeqCst); }
    if !out.is_null() { unsafe { (*out).size = std::mem::size_of::<OrcaHookResult>() as u32; (*out).version = 1; (*out).action = 0; } }
    0
}
extern "C" fn replace_get_time_tramp(ctx: *mut OrcaCpuContext, out: *mut OrcaHookResult, next: Option<extern "C" fn(*mut OrcaCpuContext) -> i32>, _ud: *mut c_void) -> i32 {
    let outcome = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
        if ctx.is_null() { append_log_line("rust-demo replace Slic3r::Utils::get_current_time_utc ctx=null"); return -1; }
        let rip_before = unsafe { (*ctx).rip };
        append_log_line(&format!("rust-demo replace Slic3r::Utils::get_current_time_utc entry rip=0x{:x}", rip_before));
        let st = if let Some(n) = next { n(ctx) } else { append_log_line("rust-demo replace Slic3r::Utils::get_current_time_utc next=null"); -1 };
        let c = unsafe { &*ctx };
        let msg = format!("rust-demo replace Slic3r::Utils::get_current_time_utc next_ret_status={} rax=0x{:x} rip=0x{:x} rsp=0x{:x}", st, c.rax, c.rip, c.rsp);
        append_log_line(&msg);
        st
    }));
    let ret = match outcome { Ok(v) => v, Err(_) => { append_log_line("rust-demo panic_contained hook=replace_get_time"); -1 } };
    if !out.is_null() { unsafe { (*out).size = std::mem::size_of::<OrcaHookResult>() as u32; (*out).version = 1; (*out).action = 0; } }
    ret
}
extern "C" fn before_print_info_tramp(ctx: *mut OrcaCpuContext, out: *mut OrcaHookResult, _ud: *mut c_void) -> i32 {
    let res = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
        if ctx.is_null() { append_log_line("rust-demo before Slic3r::Model::print_info ctx=null"); return; }
        let c = unsafe { &*ctx };
        let msg = format!("rust-demo before Slic3r::Model::print_info this_rcx=0x{:x} rip=0x{:x} rsp=0x{:x} rax=0x{:x} rflags=0x{:x}", c.rcx, c.rip, c.rsp, c.rax, c.rflags);
        append_log_line(&msg);
    }));
    if res.is_err() { append_log_line("rust-demo panic_contained hook=before_print_info"); }
    if !out.is_null() { unsafe { (*out).size = std::mem::size_of::<OrcaHookResult>() as u32; (*out).version = 1; (*out).action = 0; } }
    0
}
extern "C" fn panic_demo_tramp(ctx: *mut OrcaCpuContext, out: *mut OrcaHookResult, _ud: *mut c_void) -> i32 {
    if PANIC_DISABLED.load(Ordering::SeqCst) {
        append_log_line("rust-demo panic_demo skipped already disabled");
        if !out.is_null() { unsafe { (*out).size = std::mem::size_of::<OrcaHookResult>() as u32; (*out).version = 1; (*out).action = 0; } }
        return 0;
    }
    let res = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
        panic!("intentional panic for demo");
    }));
    if res.is_err() {
        append_log_line("rust-demo panic_contained hook=panic_demo target=Slic3r::Utils::get_current_time_utc");
        PANIC_DISABLED.store(true, Ordering::SeqCst);
        append_log_line("rust-demo panic hook disabled for subsequent calls - process survived");
    } else {
        append_log_line("rust-demo panic_demo no panic");
    }
    if !out.is_null() { unsafe { (*out).size = std::mem::size_of::<OrcaHookResult>() as u32; (*out).version = 1; (*out).action = 0; } }
    if !ctx.is_null() {
        let c = unsafe { &*ctx };
        append_log_line(&format!("rust-demo panic_demo tramp called rip=0x{:x} rsp=0x{:x}", c.rip, c.rsp));
    }
    0
}
static HOOK_ID_BEFORE_GET_TIME: &[u8] = b"rust.before_get_time\0";
static HOOK_ID_REPLACE_GET_TIME: &[u8] = b"rust.replace_get_time\0";
static HOOK_ID_BEFORE_PRINT_INFO: &[u8] = b"rust.before_print_info\0";
static HOOK_ID_PANIC: &[u8] = b"rust.panic_demo\0";
static SYM_GET_TIME: &[u8] = b"Slic3r::Utils::get_current_time_utc\0";
static SYM_PRINT_INFO: &[u8] = b"Slic3r::Model::print_info\0";
#[no_mangle] pub extern "C" fn orca_plugin_entry_v1(host: *const OrcaHostApiV1) -> i32 {
    G_HOST.store(host as *mut OrcaHostApiV1, Ordering::SeqCst);
    let args: Vec<String> = std::env::args().collect();
    append_log_line(&format!("plugin_load id=com.orca.rust-example version=0.1.0 language=rust runtime=native args={:?} ORCA_DATA_DIR={:?}", args, std::env::var("ORCA_DATA_DIR").unwrap_or_default()));
    if host.is_null() { append_log_line("rust-demo entry host=null"); return -1; }
    let h = unsafe { &*host };
    if h.version != 1 { append_log_line(&format!("rust-demo entry bad host version {}", h.version)); return -2; }
    if h.install_hook.is_none() { append_log_line("rust-demo entry host.install_hook=null"); return -3; }
    let install = h.install_hook.unwrap();
    let mut installed: Vec<*mut c_void> = Vec::new();
    let mut install_one = |hook_id: *const i8, sym: *const i8, point: u32, kind: u32, prio: u32, cb: *mut c_void| -> i32 {
        let req = OrcaHookRequest { size: std::mem::size_of::<OrcaHookRequest>() as u32, version: 1, hook_id, target_symbol_id: sym, target_rva: 0, point, kind, priority: prio, u_offset_rva: 0, _u_pad: 0, _u_pad2: 0, callback: cb, user_data: std::ptr::null_mut(), };
        let mut handle: *mut c_void = std::ptr::null_mut();
        let st = unsafe { install(&req as *const _, &mut handle as *mut _) };
        let sym_str = unsafe { std::ffi::CStr::from_ptr(sym).to_string_lossy().into_owned() };
        let hk_str = unsafe { std::ffi::CStr::from_ptr(hook_id).to_string_lossy().into_owned() };
        if st == 0 { append_log_line(&format!("install_ok symbol={} point={} kind={} priority={} hook={}", sym_str, point, kind, prio, hk_str)); installed.push(handle); } else { append_log_line(&format!("install_failed symbol={} hook={} status={} {} rva=0 point={} kind={} prio={}", sym_str, hk_str, st, hook_status_str(st), point, kind, prio)); if let Some(log_fn) = h.log { let msg = format!("rust install_failed {} status {}\0", hk_str, st); unsafe { log_fn(2, msg.as_ptr() as *const i8) }; } }
        st
    };
    let mut overall = 0;
    let st1 = install_one(HOOK_ID_BEFORE_GET_TIME.as_ptr() as *const i8, SYM_GET_TIME.as_ptr() as *const i8, 0, 0, 1300, before_get_time_tramp as *mut c_void);
    if st1 != 0 { overall = st1; }
    let st2 = install_one(HOOK_ID_REPLACE_GET_TIME.as_ptr() as *const i8, SYM_GET_TIME.as_ptr() as *const i8, 0, 2, 1150, replace_get_time_tramp as *mut c_void);
    if st2 != 0 && overall == 0 { overall = st2; }
    let st3 = install_one(HOOK_ID_BEFORE_PRINT_INFO.as_ptr() as *const i8, SYM_PRINT_INFO.as_ptr() as *const i8, 0, 0, 1100, before_print_info_tramp as *mut c_void);
    if st3 != 0 && overall == 0 { overall = st3; }
    let st4 = install_one(HOOK_ID_PANIC.as_ptr() as *const i8, SYM_GET_TIME.as_ptr() as *const i8, 0, 0, 900, panic_demo_tramp as *mut c_void);
    if st4 != 0 && overall == 0 { overall = st4; }
    if overall != 0 { append_log_line(&format!("rust-demo entry install overall status {}", overall)); if let Some(rm) = h.remove_hook { for handle in installed { let _ = unsafe { rm(handle) }; } } return overall; }
    append_log_line("rust-demo entry install_all_ok");
    0
}
#[no_mangle] pub extern "C" fn orca_plugin_exit_v1() -> i32 { PANIC_DISABLED.store(false, Ordering::SeqCst); append_log_line("plugin_unload id=com.orca.rust-example"); 0 }
