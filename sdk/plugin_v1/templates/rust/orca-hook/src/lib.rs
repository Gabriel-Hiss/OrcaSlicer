// orca-hook — safe wrappers over the C ABI, borrowed newtypes, raw escape.
// Generated per-build bindings live in generated/plugin-sdk/<build-id>/rust/orca-hook/src/generated/
// This crate itself is static (no generation) and compiles standalone for IDE.

pub mod raw;
pub mod context;
pub mod handle;
pub mod target;
pub mod hooks;
pub mod generated; // filled by emitter; placeholder below keeps crate compilable

pub use raw::RawHook;
pub use context::CpuContext;
pub use handle::{Borrowed, BorrowedMut};
pub use target::{HookTarget, HookPoint, HookKind, Next};
pub use hooks::register_hook;

use std::panic::{self, AssertUnwindSafe};

// Catch unwind boundary for Rust hooks — panic must not cross C ABI.
// The host disables only that hook for the session; stack/error preserved.
pub fn catch_hook<F, R>(f: F) -> Option<R>
where
    F: FnOnce() -> R + panic::UnwindSafe,
{
    match panic::catch_unwind(AssertUnwindSafe(f)) {
        Ok(v) => Some(v),
        Err(payload) => {
            // Best-effort diagnostic; host logs if available.
            let _ = payload;
            None
        }
    }
}

pub use orca_hook_macros::{hook, before, after, replace, at};

// ABI version — must match sdk/plugin_v1/abi/orca_hook_api.h ORCA_HOOK_ABI_VERSION == 1
pub const ABI_VERSION: u32 = 1;

// Build id for this SDK — emitter fills via env! or generated file.
pub const BUILD_ID: &str = "@ORCA_BUILD_ID@";
pub const HOOK_ABI: u32 = 1;

// Opaque host table forward (defined in orca-sys / generated bindings).
#[repr(C)]
pub struct HostApi {
    _priv: [u8; 0],
}
