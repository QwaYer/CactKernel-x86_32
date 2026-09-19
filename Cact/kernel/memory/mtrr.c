/*
 * MTRR save/restore across S3.
 *
 * The MTRRs are a per-CPU MSR block programmed by firmware before the kernel
 * starts, and they take precedence over PAT for the uncacheable case: an
 * address covered by no variable range takes the default type, and if that
 * default is UC no PAT entry or PTE bit can make it cacheable again.
 *
 * A platform reset (which is what an S3 wake performs) drops the block back to
 * its architectural default — default type UC, no variable ranges — and the
 * firmware that programmed it is not re-run.  Every MMIO address in the PCI
 * hole, the framebuffer above all, then becomes uncacheable: the shadow->FB
 * copy in fb_flush() degrades from write-combined speed to per-dword device
 * traffic (a 3 MB flush goes from ~0 ms to ~6 s under KVM).  Restoring the
 * snapshot is the OS's job, exactly like PCI config space.
 */

#include "mtrr.h"
#include "kernel.h"
#include "klib.h"

#define MSR_MTRRCAP        0xFE
#define MSR_MTRR_PHYSBASE0 0x200
#define MSR_MTRR_DEF_TYPE  0x2FF

#define MTRR_DEF_TYPE_ENABLE (1ull << 11)

/* Architectural maximum for the number of variable ranges (SDM: 10). */
#define MTRR_MAX_VAR 10

static uint64_t mtrr_base[MTRR_MAX_VAR];
static uint64_t mtrr_mask[MTRR_MAX_VAR];
static uint32_t mtrr_var_count;
static uint64_t mtrr_def_type;
static int      mtrr_saved;

static inline uint64_t mtrr_rdmsr(uint32_t msr)
{
    uint32_t lo, hi;
    __asm__ __volatile__("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}

static inline void mtrr_wrmsr(uint32_t msr, uint64_t val)
{
    uint32_t lo = (uint32_t)val, hi = (uint32_t)(val >> 32);
    __asm__ __volatile__("wrmsr" :: "c"(msr), "a"(lo), "d"(hi));
}

static inline uint32_t mtrr_range_of(uint32_t i)
{
    return MSR_MTRR_PHYSBASE0 + 2u * i;
}

void mtrr_save(void)
{
    uint32_t n = (uint32_t)mtrr_rdmsr(MSR_MTRRCAP) & 0xFFu;
    if (n > MTRR_MAX_VAR)
        n = MTRR_MAX_VAR;

    mtrr_var_count = n;
    mtrr_def_type  = mtrr_rdmsr(MSR_MTRR_DEF_TYPE);
    for (uint32_t i = 0; i < n; i++) {
        mtrr_base[i] = mtrr_rdmsr(mtrr_range_of(i));
        mtrr_mask[i] = mtrr_rdmsr(mtrr_range_of(i) + 1u);
    }
    mtrr_saved = 1;

    pr_info("  %-11s : %u variable range(s), default type 0x%x — snapshot kept\n",
            "mtrr", (unsigned)n, (unsigned)(mtrr_def_type & 0xFFu));
}

void mtrr_restore(void)
{
    if (!mtrr_saved)
        return;

    /* Skip the write-back when the block still holds the snapshot. */
    int same = (mtrr_rdmsr(MSR_MTRR_DEF_TYPE) == mtrr_def_type);
    for (uint32_t i = 0; same && i < mtrr_var_count; i++) {
        same = mtrr_rdmsr(mtrr_range_of(i)) == mtrr_base[i]
            && mtrr_rdmsr(mtrr_range_of(i) + 1u) == mtrr_mask[i];
    }
    if (same)
        return;

    /* The SDM requires the enable bit to be clear while the ranges are
     * rewritten, and a cache flush once they are back in place. */
    mtrr_wrmsr(MSR_MTRR_DEF_TYPE, mtrr_def_type & ~MTRR_DEF_TYPE_ENABLE);
    for (uint32_t i = 0; i < mtrr_var_count; i++) {
        mtrr_wrmsr(mtrr_range_of(i), mtrr_base[i]);
        mtrr_wrmsr(mtrr_range_of(i) + 1u, mtrr_mask[i]);
    }
    mtrr_wrmsr(MSR_MTRR_DEF_TYPE, mtrr_def_type);

    __asm__ __volatile__("wbinvd" ::: "memory");

    pr_info("  %-11s : %u range(s) restored after the sleep\n",
            "mtrr", (unsigned)mtrr_var_count);
}
