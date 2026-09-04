# Orca Hook X64 Stubs - Linux GAS, SysV AMD64 ABI.
# Each stub builds a CpuContext on the stack and calls the C++ dispatcher via PLT.
# Stages: frame construction with 16-byte alignment; saving scratch registers
# before reuse; CpuContext with RIP/RSP derived from the caller frame plus
# XMM0-15 and RFLAGS; dispatcher call (rdi target, rsi context, rdx trampoline
# slot); write-back of the dispatcher-modified registers; frame teardown.
# Layout matches sdk/plugin_v1/abi/orca_hook_api.h orca_cpu_context_t (size 408, version 1).
# Offsets: size 0, version 4, rax 8, rbx 16, rcx 24, rdx 32, rsi 40, rdi 48, rbp 56, rsp 64,
# r8 72, r9 80, r10 88, r11 96, r12 104, r13 112, r14 120, r15 128, rip 136, rflags 144,
# xmm0 152, xmm1 168, xmm2 184, xmm3 200, xmm4 216, xmm5 232, xmm6 248, xmm7 264,
# xmm8 280, xmm9 296, xmm10 312, xmm11 328, xmm12 344, xmm13 360, xmm14 376, xmm15 392.

.intel_syntax noprefix
.text

.extern orca_hook_inline_dispatch
.extern orca_hook_mid_dispatch

.globl orca_hook_inline_stub
.type orca_hook_inline_stub, @function
orca_hook_inline_stub:
# Prologue: build a frame and align the stack to 16 bytes.
    push rbp
    mov  rbp, rsp
    sub  rsp, 1088
    and  rsp, -16
# Save rax/r10/r11/rsi before pushfq and the context pointer reuse them.
    mov  QWORD PTR [rsp+1000], rax
    mov  QWORD PTR [rsp+1008], r10
    mov  QWORD PTR [rsp+1016], r11
    mov  QWORD PTR [rsp+1024], rsi
    pushfq
    pop  rax
    mov  QWORD PTR [rsp+1032], rax
# CpuContext: RIP from the caller return address, RSP recomputed past the
# frame, then GPRs, saved RFLAGS, and XMM0-15 at their context offsets.
    lea  rsi, [rsp+32]
    mov  DWORD PTR [rsi+0], 408
    mov  DWORD PTR [rsi+4], 1
    mov  rax, QWORD PTR [rsp+1000]
    mov  [rsi+8], rax
    mov  [rsi+16], rbx
    mov  [rsi+24], rcx
    mov  [rsi+32], rdx
    mov  rax, QWORD PTR [rsp+1024]
    mov  [rsi+40], rax
    mov  [rsi+48], rdi
    mov  rax, QWORD PTR [rbp]
    mov  [rsi+56], rax
    lea  rax, [rbp+16]
    mov  [rsi+64], rax
    mov  [rsi+72], r8
    mov  [rsi+80], r9
    mov  rax, QWORD PTR [rsp+1008]
    mov  [rsi+88], rax
    mov  rax, QWORD PTR [rsp+1016]
    mov  [rsi+96], rax
    mov  [rsi+104], r12
    mov  [rsi+112], r13
    mov  [rsi+120], r14
    mov  [rsi+128], r15
    mov  rax, [rbp+8]
    mov  [rsi+136], rax
    mov  rax, [rsp+1032]
    mov  [rsi+144], rax
    movdqa [rsi+152], xmm0
    movdqa [rsi+168], xmm1
    movdqa [rsi+184], xmm2
    movdqa [rsi+200], xmm3
    movdqa [rsi+216], xmm4
    movdqa [rsi+232], xmm5
    movdqa [rsi+248], xmm6
    movdqa [rsi+264], xmm7
    movdqa [rsi+280], xmm8
    movdqa [rsi+296], xmm9
    movdqa [rsi+312], xmm10
    movdqa [rsi+328], xmm11
    movdqa [rsi+344], xmm12
    movdqa [rsi+360], xmm13
    movdqa [rsi+376], xmm14
    movdqa [rsi+392], xmm15
# Dispatcher call, SysV: rdi carries target_info, rsi the context, rdx the
# trampoline slot; the call goes through the PLT.
    mov  rdi, r11
    lea  rdx, [rbp-8]
    mov  QWORD PTR [rbp-8], 0
    call orca_hook_inline_dispatch@PLT
# Write back the dispatcher-modified registers, then tear down the frame.
    mov  rax, [rsi+8]
    movdqa xmm0, [rsi+152]
    movdqa xmm1, [rsi+168]
    mov  rbx, [rsi+16]
    mov  r12, [rsi+104]
    mov  r13, [rsi+112]
    mov  r14, [rsi+120]
    mov  r15, [rsi+128]
    mov  rsp, rbp
    pop  rbp
    ret
    .size orca_hook_inline_stub, .-orca_hook_inline_stub

.globl orca_hook_mid_stub
.type orca_hook_mid_stub, @function
orca_hook_mid_stub:
    push rbp
    mov  rbp, rsp
    sub  rsp, 1088
    and  rsp, -16
    mov  r11, r10
    mov  QWORD PTR [rsp+1000], rax
    mov  QWORD PTR [rsp+1008], rsi
.globl orca_hook_mid_stub
.type orca_hook_mid_stub, @function
orca_hook_mid_stub:
# Prologue: build a frame and align the stack to 16 bytes.
    push rbp
    mov  rbp, rsp
    sub  rsp, 1088
    and  rsp, -16
    mov  QWORD PTR [rsp+1000], rax
    mov  QWORD PTR [rsp+1008], r10
    mov  QWORD PTR [rsp+1016], r11
    mov  QWORD PTR [rsp+1024], rsi
    pushfq
    pop  rax
    mov  QWORD PTR [rsp+1032], rax
# CpuContext: RIP from the caller return address, RSP recomputed past the
# frame, then GPRs, saved RFLAGS, and XMM0-15 at their context offsets.
    lea  rsi, [rsp+32]
    mov  DWORD PTR [rsi+0], 408
    mov  DWORD PTR [rsi+4], 1
    mov  rax, QWORD PTR [rsp+1000]
    mov  [rsi+8], rax
    mov  [rsi+16], rbx
    mov  [rsi+24], rcx
    mov  [rsi+32], rdx
    mov  rax, QWORD PTR [rsp+1024]
    mov  [rsi+40], rax
    mov  [rsi+48], rdi
    mov  rax, QWORD PTR [rbp]
    mov  [rsi+56], rax
    lea  rax, [rbp+16]
    mov  [rsi+64], rax
    mov  [rsi+72], r8
    mov  [rsi+80], r9
    mov  rax, QWORD PTR [rsp+1008]
    mov  [rsi+88], rax
    mov  rax, QWORD PTR [rsp+1016]
    mov  [rsi+96], rax
    mov  [rsi+112], r13
    mov  [rsi+120], r14
    mov  [rsi+128], r15
    mov  rax, [rbp+8]
    mov  [rsi+136], rax
    mov  rax, [rsp+1024]
    mov  [rsi+144], rax
    movdqa [rsi+152], xmm0
    movdqa [rsi+168], xmm1
    movdqa [rsi+184], xmm2
    movdqa [rsi+200], xmm3
    movdqa [rsi+216], xmm4
    movdqa [rsi+232], xmm5
    movdqa [rsi+248], xmm6
    movdqa [rsi+264], xmm7
    movdqa [rsi+280], xmm8
    movdqa [rsi+296], xmm9
    movdqa [rsi+312], xmm10
    movdqa [rsi+328], xmm11
    movdqa [rsi+344], xmm12
    movdqa [rsi+360], xmm13
    movdqa [rsi+376], xmm14
    movdqa [rsi+392], xmm15
# Dispatcher call, SysV: rdi carries target_info, rsi the context; the call
# goes through the PLT. No trampoline slot: traps resume in place.
    mov  rdi, r11
    call orca_hook_mid_dispatch@PLT
# Write back the dispatcher-modified registers, then tear down the frame.
    mov  rax, [rsi+8]
    movdqa xmm0, [rsi+152]
    mov  rsp, rbp
    pop  rbp
    ret
    .size orca_hook_mid_stub, .-orca_hook_mid_stub


.globl orca_hook_call_trampoline
.type orca_hook_call_trampoline, @function
orca_hook_call_trampoline:
# Trampoline caller: preserve callee-saved registers, then marshal the
# context into SysV argument registers (rdi/rsi/rdx/rcx/r8/r9, xmm0-7).
    push rbp
    mov  rbp, rsp
    push rbx
    push r12
    push r13
    push r14
    push r15
    sub  rsp, 8
    mov  r12, rdi
    mov  r13, rsi
    mov  rdi, [r13+48]
    mov  rsi, [r13+40]
    mov  rdx, [r13+32]
    mov  rcx, [r13+24]
    mov  r8,  [r13+72]
    mov  r9,  [r13+80]
    movdqa xmm0, [r13+152]
    movdqa xmm1, [r13+168]
    movdqa xmm2, [r13+184]
    movdqa xmm3, [r13+200]
    movdqa xmm4, [r13+216]
    movdqa xmm5, [r13+232]
    movdqa xmm6, [r13+248]
    movdqa xmm7, [r13+264]
    call r12
# Write the results back into the context, restore, and return.
    mov  [r13+8], rax
    mov  [r13+32], rdx
    movdqa [r13+152], xmm0
    movdqa [r13+168], xmm1
    add  rsp, 8
    pop  r15
    pop  r14
    pop  r13
    pop  r12
    pop  rbx
    pop  rbp
    ret
    .size orca_hook_call_trampoline, .-orca_hook_call_trampoline
