use crate::target::{HookKind, HookPoint};
use parking_lot::Mutex;
use std::sync::LazyLock;

static REGISTRY: LazyLock<Mutex<Vec<HookRegistration>>> = LazyLock::new(|| Mutex::new(Vec::new()));

#[derive(Clone, Copy, Debug)]
pub struct HookRegistration {
    pub symbol_id: &'static str,
    pub rva: u32,
    pub point: HookPoint,
    pub kind: HookKind,
    pub priority: i32,
    pub ordinal: u32,
    pub offset_rva: u32,
    pub vtable_index: u32,
    pub import_module: Option<&'static str>,
    pub import_name: Option<&'static str>,
    pub trampoline: *mut std::ffi::c_void,
}

unsafe impl Send for HookRegistration {}
unsafe impl Sync for HookRegistration {}

pub fn register_hook(reg: HookRegistration) {
    REGISTRY.lock().push(reg);
}
pub fn take_registrations() -> Vec<HookRegistration> {
    std::mem::take(&mut *REGISTRY.lock())
}

// Re-exported for proc-macro generated code to call.
