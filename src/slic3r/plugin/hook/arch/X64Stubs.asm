; Orca Hook X64 Stubs - Windows MASM, Microsoft x64 ABI.
; Each stub builds a CpuContext on the stack and calls the C++ dispatcher.
; Stages: frame construction with 16-byte alignment; saving original volatile
; registers before scratch use; CpuContext with RIP/RSP derived from the caller
; frame plus XMM0-15 and RFLAGS; dispatcher call with shadow space; restoring
; the dispatcher-modified context; frame teardown and return.
; Layout matches orca_cpu_context_t (size 408, version 1). HookMemory flushes
; the icache after any code patch; stubs themselves are never patched.

.code

extern orca_hook_inline_dispatch:proc

extern orca_hook_mid_dispatch:proc
extern orca_hook_call_trampoline:proc
orca_hook_inline_stub proc
; Prologue: build a frame and align the stack to 16 bytes.
    push rbp
    mov  rbp, rsp
    sub  rsp, 440h
    and  rsp, 0FFFFFFFFFFFFFFF0h
    ; Save original RAX and RDX before they are clobbered by pushfq/lea
    mov  [rsp+8], rax
    mov  [rsp+16], rdx
    pushfq
    pop  rax
    mov  [rsp + 300h], rax
; CpuContext: RIP from the caller return address, RSP recomputed past the
; frame, then GPRs, saved RFLAGS, and XMM0-15 at their context offsets.
    lea  rdx, [rsp + 32]
    mov  dword ptr [rdx + 0], 408
    mov  dword ptr [rdx + 4], 1
    mov  rax, [rsp+8]
    mov  [rdx + 8], rax
    mov  [rdx + 16], rbx
    mov  [rdx + 24], rcx
    mov  rax, [rsp+16]
    mov  [rdx + 32], rax
    mov  [rdx + 40], rsi
    mov  [rdx + 48], rdi
    mov  rax, [rbp]
    mov  [rdx + 56], rax
    lea  rax, [rbp+16]
    mov  [rdx + 64], rax
    mov  [rdx + 72], r8
    mov  [rdx + 80], r9
    mov  [rdx + 88], r10
    mov  [rdx + 96], r11
    mov  [rdx + 104], r12
    mov  [rdx + 112], r13
    mov  [rdx + 120], r14
    mov  [rdx + 128], r15
    mov  rax, [rbp+8]
    mov  [rdx + 136], rax
    mov  rax, [rsp + 300h]
    mov  [rdx + 144], rax
    movdqa [rdx + 152], xmm0
    movdqa [rdx + 168], xmm1
    movdqa [rdx + 184], xmm2
    movdqa [rdx + 200], xmm3
    movdqa [rdx + 216], xmm4
    movdqa [rdx + 232], xmm5
    movdqa [rdx + 248], xmm6
    movdqa [rdx + 264], xmm7
    movdqa [rdx + 280], xmm8
    movdqa [rdx + 296], xmm9
    movdqa [rdx + 312], xmm10
    movdqa [rdx + 328], xmm11
    movdqa [rdx + 344], xmm12
    movdqa [rdx + 360], xmm13
    movdqa [rdx + 376], xmm14
    movdqa [rdx + 392], xmm15
; Dispatcher call, Microsoft x64: rcx still carries target_info, rdx the
; context, r8 the trampoline slot; 32 bytes of shadow space.
    sub  rsp, 32
    lea  r8, [rbp - 8]
    call orca_hook_inline_dispatch
    add  rsp, 32
; Restore the dispatcher-modified context: RFLAGS, XMM0-15, then GPRs.
    mov r10, rdx
    mov rax, [r10+144]
    push rax
    popfq
    movdqa xmm0, [r10 + 152]
    movdqa xmm1, [r10 + 168]
    movdqa xmm2, [r10 + 184]
    movdqa xmm3, [r10 + 200]
    movdqa xmm4, [r10 + 216]
    movdqa xmm5, [r10 + 232]
    movdqa xmm6, [r10 + 248]
    movdqa xmm7, [r10 + 264]
    movdqa xmm8, [r10 + 280]
    movdqa xmm9, [r10 + 296]
    movdqa xmm10,[r10 + 312]
    movdqa xmm11,[r10 + 328]
    movdqa xmm12,[r10 + 344]
    movdqa xmm13,[r10 + 360]
    movdqa xmm14,[r10 + 376]
    movdqa xmm15,[r10 + 392]
    mov  rax, [r10 + 8]
    mov  rbx, [r10 + 16]
    mov  rcx, [r10 + 24]
    mov  rdx, [r10 + 32]
    mov  rsi, [r10 + 40]
    mov  rdi, [r10 + 48]
    mov  r8, [r10 + 72]
    mov  r9, [r10 + 80]
    mov  r11, [r10 + 96]
    mov  r12, [r10 + 104]
    mov  r13, [r10 + 112]
    mov  r14, [r10 + 120]
    mov  r15, [r10 + 128]
    mov  r10, [r10 + 88]
; Return: tear down the frame and resume the caller.
    mov  rsp, rbp
    pop  rbp
    ret
orca_hook_inline_stub endp

orca_hook_mid_stub proc
; Prologue: build a frame and align the stack to 16 bytes.
    push rbp
    mov  rbp, rsp
    sub  rsp, 440h
    and  rsp, 0FFFFFFFFFFFFFFF0h
; Save original RAX and RDX before pushfq/lea clobber them.
    mov  [rsp+8], rax
    mov  [rsp+16], rdx
    pushfq
    pop  rax
    mov  [rsp+300h], rax
; CpuContext: RIP from the caller return address, RSP recomputed past the
; frame, then GPRs, saved RFLAGS, and XMM0-15 at their context offsets.
    lea  rdx, [rsp+32]
    mov  dword ptr [rdx], 408
    mov  dword ptr [rdx+4], 1
    mov  rax, [rsp+8]
    mov  [rdx+8], rax
    mov  [rdx+16], rbx
    mov  [rdx+24], rcx
    mov  rax, [rsp+16]
    mov  [rdx+32], rax
    mov  [rdx+40], rsi
    mov  [rdx+48], rdi
    mov  rax, [rbp]
    mov  [rdx+56], rax
    lea  rax, [rbp+16]
    mov  [rdx+64], rax
    mov  [rdx+72], r8
    mov  [rdx+80], r9
    mov  [rdx+88], r10
    mov  [rdx+96], r11
    mov  [rdx+104], r12
    mov  [rdx+112], r13
    mov  [rdx+120], r14
    mov  [rdx+128], r15
    mov  rax, [rbp+8]
    mov  [rdx+136], rax
    mov  rax, [rsp+300h]
    mov  [rdx+144], rax
    movdqa [rdx+152], xmm0
    movdqa [rdx+168], xmm1
    movdqa [rdx+184], xmm2
    movdqa [rdx+200], xmm3
    movdqa [rdx+216], xmm4
    movdqa [rdx+232], xmm5
    movdqa [rdx+248], xmm6
    movdqa [rdx+264], xmm7
    movdqa [rdx+280], xmm8
    movdqa [rdx+296], xmm9
    movdqa [rdx+312], xmm10
    movdqa [rdx+328], xmm11
    movdqa [rdx+344], xmm12
    movdqa [rdx+360], xmm13
    movdqa [rdx+376], xmm14
    movdqa [rdx+392], xmm15
; Dispatcher call, Microsoft x64: rcx still carries target_info, rdx the
; context; 32 bytes of shadow space. No trampoline slot: traps resume in place.
    sub  rsp, 32
    call orca_hook_mid_dispatch
    add  rsp, 32
; Restore the dispatcher-modified context: RFLAGS, XMM0-15, then GPRs.
    mov r10, rdx
    mov rax, [r10+144]
    push rax
    popfq
    movdqa xmm0, [r10+152]
    movdqa xmm1, [r10+168]
    movdqa xmm2, [r10+184]
    movdqa xmm3, [r10+200]
    movdqa xmm4, [r10+216]
    movdqa xmm5, [r10+232]
    movdqa xmm6, [r10+248]
    movdqa xmm7, [r10+264]
    movdqa xmm8, [r10+280]
    movdqa xmm9, [r10+296]
    movdqa xmm10,[r10+312]
    movdqa xmm11,[r10+328]
    movdqa xmm12,[r10+344]
    movdqa xmm13,[r10+360]
    movdqa xmm14,[r10+376]
    movdqa xmm15,[r10+392]
    mov  rax, [r10+8]
    mov  rbx, [r10+16]
    mov  rcx, [r10+24]
    mov  rdx, [r10+32]
    mov  rsi, [r10+40]
    mov  rdi, [r10+48]
    mov  r8, [r10+72]
    mov  r9, [r10+80]
    mov  r11, [r10+96]
    mov  r12, [r10+104]
    mov  r13, [r10+112]
    mov  r14, [r10+120]
    mov  r15, [r10+128]
    mov  r10, [r10+88]
; Return: tear down the frame and resume the caller.
    mov  rsp, rbp
    pop  rbp
    ret
orca_hook_mid_stub endp

end
