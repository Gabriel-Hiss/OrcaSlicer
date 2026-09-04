// CpuContext — x64 GPR/XMM/flags/rsp/rip view over orca_cpu_context_t.
// Mirrors sdk/plugin_v1/abi/orca_hook_api.h CpuContext layout (stable, size-versioned).

#[repr(C)]
pub struct CpuContext {
    pub size: u32,
    _pad: u32,
    pub rax: u64, pub rbx: u64, pub rcx: u64, pub rdx: u64,
    pub rsi: u64, pub rdi: u64, pub rbp: u64, pub rsp: u64,
    pub r8: u64,  pub r9: u64,  pub r10: u64, pub r11: u64,
    pub r12: u64, pub r13: u64, pub r14: u64, pub r15: u64,
    pub rip: u64, pub rflags: u64,
    pub xmm: [[u8; 16]; 16],
}

impl CpuContext {
    pub fn new() -> Self {
        Self {
            size: std::mem::size_of::<Self>() as u32,
            _pad: 0,
            rax: 0, rbx: 0, rcx: 0, rdx: 0, rsi: 0, rdi: 0, rbp: 0, rsp: 0,
            r8: 0, r9: 0, r10: 0, r11: 0, r12: 0, r13: 0, r14: 0, r15: 0,
            rip: 0, rflags: 0,
            xmm: [[0; 16]; 16],
        }
    }
    #[inline] pub fn rax_mut(&mut self) -> &mut u64 { &mut self.rax }
    #[inline] pub fn rsp_mut(&mut self) -> &mut u64 { &mut self.rsp }
    #[inline] pub fn rip_mut(&mut self) -> &mut u64 { &mut self.rip }
}
