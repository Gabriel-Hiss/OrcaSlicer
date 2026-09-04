// @GENERATED@ generated.rs — per-build typed bindings, emitted by hook-sdkgen
// Emitter iterates manifest symbols where typed_binding.available==true and emits
// Rust newtypes + HookTarget impls.
// For template compilation, provide one concrete example (Slic3r::CLI::print_help)
// that all four language examples intercept.

use crate::target::{HookPoint, HookTarget};

pub struct CliPrintHelp;
impl HookTarget for CliPrintHelp {
    const SYMBOL_ID: &'static str = "Slic3r::CLI::print_help";
    const RVA: u32 = 0; // @ORCA_RVA_PRINT_HELP@ replaced per build
    const POINT: HookPoint = HookPoint::Entry;
    type Signature = fn();
}

// @GENERATED_SYMBOL_ENTRIES@ — emitter appends one impl per typed symbol.
// Example emitted:
//   pub struct FooBar; impl HookTarget for FooBar { ... type Signature = unsafe extern "C" fn(*const Foo, i32) -> bool; }
// Borrowed newtypes for UDTs:
//   pub struct BorrowedFoo(pub *const Foo); // handle, never owned
