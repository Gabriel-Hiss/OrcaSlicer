/// Typed HookTarget — Rust newtype over manifest symbol.
/// Generator emits one per symbol with typed_binding.available==true.
/// Raw symbols remain resolvable by id/RVA via RawHook only.

#[derive(Clone, Copy, Debug)]
pub enum HookPoint {
    Entry = 0,
    Return = 1,
    Invoke = 2,
    Offset = 3,
    VTable = 4,
    Iat = 5,
    Got = 6, // alias Iat on Linux (import_hook)
}
#[derive(Clone, Copy, Debug)]
pub enum HookKind {
    Before = 0,
    After = 1,
    Replace = 2,
}

/// Typed target marker — zero-sized, parameterized by signature.
/// Example (generated): `pub struct CLI_print_help; impl HookTarget for CLI_print_help { ... }`
pub trait HookTarget {
    const SYMBOL_ID: &'static str;
    const RVA: u32;
    const POINT: HookPoint;
    // For INVOKE/OFFSET/VTABLE/IAT/GOT, associated consts:
    const ORDINAL: u32 = 0;
    const OFFSET_RVA: u32 = 0;
    const VTABLE_INDEX: u32 = 0;
    const IMPORT_MODULE: Option<&'static str> = None;
    const IMPORT_NAME: Option<&'static str> = None;
    type Signature;
}

// Call-next token for replace hooks. Can be called at most once.
pub struct Next<F> {
    f: Option<F>,
    called: bool,
}
impl<F> Next<F> {
    pub fn new(f: F) -> Self { Self { f: Some(f), called: false } }
    pub fn called(&self) -> bool { self.called }
}
impl<R, F> Next<F>
where
    F: FnOnce() -> R,
{
    pub fn call(&mut self) -> Option<R> {
        if self.called { return None; }
        self.called = true;
        self.f.take().map(|f| f())
    }
}
