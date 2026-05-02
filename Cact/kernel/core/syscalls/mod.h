#ifndef SC_MOD_H
#define SC_MOD_H

#include "kernel.h"

// Registers as saved by the interrupt stub (isr_common_stub or syscall_isr)
struct syscall_frame {
    uint32_t es, ds;
    uint32_t edi, esi, ebp, esp_dummy, ebx, edx, ecx, eax;
    uint32_t eip, cs, eflags, useresp, ss;
} __attribute__((packed));

// SYS_SIGRETURN number — exported to the Rust trampoline via FFI
extern const uint32_t sys_sigreturn_num;

// Called from interrupt.asm on int 0x80
void syscall_handler(struct syscall_frame* regs);

#endif 