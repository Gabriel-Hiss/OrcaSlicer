use crate::context::CpuContext;

/// RawHook escape — all register/GPR/XMM/flags, stack pointer, sized memory ops,
/// temporary protection change and icache flush.
/// All deref / raw symbol calls / lifetime-exceeding ops are `unsafe` and
/// must be validated against manifest typed_binding.reason.
pub struct RawHook<'a> {
    ctx: &'a mut CpuContext,
    host: *const crate::HostApi,
}

impl<'a> RawHook<'a> {
    pub fn new(ctx: &'a mut CpuContext, host: *const crate::HostApi) -> Self {
        Self { ctx, host }
    }
    pub fn context(&mut self) -> &mut CpuContext {
        self.ctx
    }
    pub fn host(&self) -> *const crate::HostApi {
        self.host
    }

    /// Explicit sized read — unsafe because addr may be invalid / lifetime unchecked.
    pub unsafe fn read(&self, addr: u64, out: *mut u8, len: usize) -> bool {
        if addr == 0 || out.is_null() { return false; }
        std::ptr::copy_nonoverlapping(addr as *const u8, out, len);
        true
    }
    pub unsafe fn write(&self, addr: u64, data: *const u8, len: usize) -> bool {
        if addr == 0 || data.is_null() { return false; }
        std::ptr::copy_nonoverlapping(data, addr as *mut u8, len);
        true
    }
    pub unsafe fn protect(&self, _addr: u64, _len: usize, _prot: u32) -> bool {
        false
    }
    pub fn flush_icache(&self, _addr: u64, _len: usize) {
    }
    pub unsafe fn read_value<T: Copy>(&self, addr: u64) -> Option<T> {
        if addr == 0 { return None; }
        // Only for trivially-copyable; caller guarantees layout.
        let mut out = std::mem::MaybeUninit::<T>::uninit();
        self.read(addr, out.as_mut_ptr() as *mut u8, std::mem::size_of::<T>());
        Some(out.assume_init())
    }
}
