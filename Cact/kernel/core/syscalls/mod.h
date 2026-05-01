#ifndef SC_MOD_H
#define SC_MOD_H

#include "kernel.h"

struct syscall_frame {
    uint32_t es, ds;
    uint32_t edi, esi, ebp, esp_dummy, ebx, edx, ecx, eax;
    uint32_t eip, cs, eflags, useresp, ss;
} __attribute__((packed));

/* Номер SYS_SIGRETURN — экспортируется в Rust-трамплин через FFI */
extern const uint32_t sys_sigreturn_num;

void syscall_handler(struct syscall_frame* regs);

#endif /* SC_MOD_H */
