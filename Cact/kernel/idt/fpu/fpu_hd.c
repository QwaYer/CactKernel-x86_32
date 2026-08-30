#include "kernel.h"
#include "task.h"
#include "memory.h"

struct task_struct* volatile fpu_owner = 0;

int fpu_global_init(void) {
    uint32_t eax, ebx, ecx, edx;

    __asm__ __volatile__(
        "xor %%ecx, %%ecx\n"
        "cpuid"
        : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
        : "a"(1)
        : "memory");

    if (!(edx & (1 << 24))) {
        return -1;
    }

    // Enable the x87 FPU and SSE.  Without CR4.OSFXSR the compiler's SSE
    // sequences (e.g. movd/movq via XMM for 64-bit returns) #UD, and the lazy
    // FPU code (fxsave/fxrstor) needs OSFXSR + OSXMMEXCPT set as well.
    // Clear CR0.EM and set CR0.MP so the x87 lands on #NM (driven by TS) and
    // is never trapped in software emulation.
    __asm__ __volatile__(
        "mov %%cr0, %%eax\n\t"
        "and $~0x4, %%eax\n\t"   /* clear EM */
        "or  $0x2, %%eax\n\t"    /* set MP  */
        "mov %%eax, %%cr0\n\t"
        :: : "eax", "memory");

    __asm__ __volatile__(
        "mov %%cr4, %%eax\n\t"
        "or  $0x600, %%eax\n\t"  /* OSFXSR | OSXMMEXCPT */
        "mov %%eax, %%cr4\n\t"
        :: : "eax", "memory");

    __asm__ __volatile__("fninit");
    return 0;
}

void fpu_cleanup_task(struct task_struct* task) {
    if (fpu_owner == task) {
        fpu_owner = 0;
    }
}

void handle_lazy_fpu(void) {
    __asm__ volatile("clts");

    if (!current_task)
        return;

    if (fpu_owner == current_task)
        return;

    struct task_struct* prev = fpu_owner;
    struct task_struct* cur  = current_task;

    if (prev && prev->fpu_context_ptr) {
        __asm__ volatile("fxsave (%0)"
            : : "r"(prev->fpu_context_ptr) : "memory");
    }

    if (cur->fpu_context_ptr) {
        __asm__ volatile("fxrstor (%0)"
            : : "r"(cur->fpu_context_ptr) : "memory");
    } else {
        cur->fpu_context_ptr = kmalloc_aligned(512, 16);
        if (!cur->fpu_context_ptr)
            return;

        extern void clear_xmm_regs(void);
        clear_xmm_regs();
        __asm__ volatile("fninit");
        __asm__ volatile("fxsave (%0)"
            : : "r"(cur->fpu_context_ptr) : "memory");
    }

    fpu_owner = cur;
}
