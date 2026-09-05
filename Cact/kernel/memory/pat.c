#include "pat.h"
#include "kernel.h"
#include "klib.h"
#include "memory.h"

/* Module state — populated by pat_init(). */
static int g_pat_present = 0;

/* Kernel page directory (defined by the Rust mm crate as #[no_mangle]). */
extern uint32_t page_directory[1024];

static inline void cpuid_raw(uint32_t leaf,
                             uint32_t* eax, uint32_t* ebx,
                             uint32_t* ecx, uint32_t* edx)
{
    __asm__ __volatile__("cpuid"
        : "=a"(*eax), "=b"(*ebx), "=c"(*ecx), "=d"(*edx)
        : "a"(leaf), "c"(0));
}

static inline void cpu_invlpg(uint32_t va) {
    __asm__ __volatile__("invlpg (%0)" :: "r"(va) : "memory");
}

void pat_init(void) {
    uint32_t a, b, c, d;
    cpuid_raw(1, &a, &b, &c, &d);
    if (!(d & (1u << PAT_CPUID_EDX_BIT))) {
        pr_warn("  %-11s : unsupported (CPUID.01H:EDX.PAT=0) — framebuffer stays UC\n",
                "pat");
        return;
    }
    g_pat_present = 1;
    pr_info("  %-11s : per-PTE memory types (WC) available\n", "pat");
}

int pat_available(void) {
    return g_pat_present;
}

int pat_enable_wc_for_framebuffer(uint32_t fb_phys,
                                   uint32_t fb_pitch,
                                   uint32_t fb_height)
{
    if (!fb_phys || !fb_pitch || !fb_height)
        return -2;

    if (!g_pat_present) {
        return -1;   /* pat_init already reported the reason */
    }

    fb_phys &= ~0xFFFu;

    uint32_t fb_size = fb_pitch * fb_height;
    fb_size = (fb_size + 0xFFFu) & ~0xFFFu;

    uint32_t end = fb_phys + fb_size;

    /* Walk the kernel PD (identity-mapped). Process PDs in the upper half
     * share these same page tables, so a single pass suffices. */
    for (uint32_t va = fb_phys; va < end; va += PAGE_SIZE) {
        uint32_t pdi = (va >> 22) & 0x3FF;
        uint32_t pti = (va >> 12) & 0x3FF;
        uint32_t pde = page_directory[pdi];
        if (!(pde & PAGE_PRESENT))
            continue;

        uint32_t* pt = (uint32_t*)(pde & ~0xFFFu);
        if (!(pt[pti] & PAGE_PRESENT))
            continue;

        /* Set PAT bit, clear PCD|PWT → (PAT=1, PCD=0, PWT=0) selects
         * PAT entry 4 = Write-Combining (WC). */
        pt[pti] |=  PAGE_PAT;
        pt[pti] &= ~(uint32_t)(PAGE_PCD | PAGE_PWT);
        cpu_invlpg(va);
    }

    return 0;   /* success reported by kernel.c's framebuffer line */
}
