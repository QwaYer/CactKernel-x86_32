#ifndef CACT_SMP_H
#define CACT_SMP_H

#include <stdint.h>

/* C-facing ABI for the Rust SMP bring-up (sched/src/smp.rs). The per-CPU
 * GDT/TSS environment, trampoline staging and INIT-SIPI sequence live in
 * Rust; this header only declares the stable entry points. */

int smp_init(void);
int smp_self_cpu(void);
int smp_cpu_online(uint32_t cpu);

#endif
