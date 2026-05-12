#include "mtrr.h"
#include "kernel.h"
#include "klib.h"
#include "memory.h"

/* ------------------------------------------------------------------------ *
 *  MSR addresses (Intel SDM Vol 4)
 * ------------------------------------------------------------------------ */
#define IA32_MTRRCAP             0x000000FE
#define IA32_MTRR_DEF_TYPE       0x000002FF
#define IA32_MTRR_PHYSBASE(n)    (0x00000200u + 2u * (n))
#define IA32_MTRR_PHYSMASK(n)    (0x00000201u + 2u * (n))

/* IA32_MTRRCAP bits */
#define MTRRCAP_VCNT_MASK        0xFFu          /* bits  7:0  — variable range count   */
#define MTRRCAP_FIX_BIT          (1u << 8)      /* fixed-range MTRRs supported         */
#define MTRRCAP_WC_BIT           (1u << 10)     /* Write-Combining memory type allowed */
#define MTRRCAP_SMRR_BIT         (1u << 11)     /* SMRR supported                      */

/* IA32_MTRR_DEF_TYPE bits */
#define MTRR_DEF_TYPE_TYPE_MASK  0xFFu          /* bits  7:0  — default memory type    */
#define MTRR_DEF_TYPE_FE         (1u << 10)     /* Fixed-range MTRRs enable            */
#define MTRR_DEF_TYPE_E          (1u << 11)     /* Master MTRR enable                  */

/* PhysMask register: V (valid) bit */
#define MTRR_PHYSMASK_VALID      (1u << 11)

/* CR0 / CR4 bits we touch during the MTRR programming sequence. */
#define CR0_NW                   (1u << 29)
#define CR0_CD                   (1u << 30)
#define CR4_PGE                  (1u << 7)

/* ------------------------------------------------------------------------ *
 *  Module state — populated by mtrr_init()
 * ------------------------------------------------------------------------ */
static int      g_mtrr_present     = 0;
static int      g_wc_supported     = 0;
static int      g_variable_count   = 0;
static uint32_t g_maxphysaddr_bits = 36;   /* conservative default per SDM */

/* Kernel page directory — defined by the Rust mm crate (#[no_mangle]) and
 * also referenced by interrupt.asm and xhci.c via the same extern. We use
 * it here to clear PCD/PWT on the framebuffer's PTEs after programming the
 * MTRR; because process PDs in the upper half share these same page tables,
 * the change is automatically visible under any CR3. */
extern uint32_t page_directory[1024];

/* ------------------------------------------------------------------------ *
 *  Low-level CPU helpers (inline asm)
 * ------------------------------------------------------------------------ */

static inline void cpuid_raw(uint32_t leaf,
                             uint32_t* eax, uint32_t* ebx,
                             uint32_t* ecx, uint32_t* edx)
{
    __asm__ __volatile__("cpuid"
        : "=a"(*eax), "=b"(*ebx), "=c"(*ecx), "=d"(*edx)
        : "a"(leaf), "c"(0));
}

static inline uint64_t rdmsr(uint32_t msr) {
    uint32_t lo, hi;
    __asm__ __volatile__("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | (uint64_t)lo;
}

static inline void wrmsr(uint32_t msr, uint64_t val) {
    uint32_t lo = (uint32_t)val;
    uint32_t hi = (uint32_t)(val >> 32);
    __asm__ __volatile__("wrmsr" :: "c"(msr), "a"(lo), "d"(hi));
}

static inline void cpu_wbinvd(void) {
    __asm__ __volatile__("wbinvd" ::: "memory");
}

static inline void cpu_invlpg(uint32_t va) {
    __asm__ __volatile__("invlpg (%0)" :: "r"(va) : "memory");
}

static inline uint32_t read_cr0(void) {
    uint32_t v; __asm__ __volatile__("mov %%cr0, %0" : "=r"(v)); return v;
}
static inline void write_cr0(uint32_t v) {
    __asm__ __volatile__("mov %0, %%cr0" :: "r"(v) : "memory");
}
static inline uint32_t read_cr3(void) {
    uint32_t v; __asm__ __volatile__("mov %%cr3, %0" : "=r"(v)); return v;
}
static inline void write_cr3(uint32_t v) {
    __asm__ __volatile__("mov %0, %%cr3" :: "r"(v) : "memory");
}
static inline uint32_t read_cr4(void) {
    uint32_t v; __asm__ __volatile__("mov %%cr4, %0" : "=r"(v)); return v;
}
static inline void write_cr4(uint32_t v) {
    __asm__ __volatile__("mov %0, %%cr4" :: "r"(v) : "memory");
}

/* Save EFLAGS and clear IF. Returns the original EFLAGS for restore. */
static inline uint32_t irq_save_cli(void) {
    uint32_t f;
    __asm__ __volatile__("pushfl\n\tpopl %0\n\tcli" : "=r"(f) :: "memory");
    return f;
}
static inline void irq_restore(uint32_t f) {
    __asm__ __volatile__("pushl %0\n\tpopfl" :: "r"(f) : "memory", "cc");
}

/* ------------------------------------------------------------------------ *
 *  MTRR programming sequence — Intel SDM Vol 3A § 11.11.8
 * ------------------------------------------------------------------------ */

typedef struct {
    uint32_t cr0;
    uint32_t cr4;
    uint64_t def_type;
    uint32_t eflags;
} mtrr_prep_state_t;

static void mtrr_prepare(mtrr_prep_state_t* st) {
    st->eflags = irq_save_cli();
    st->cr0    = read_cr0();
    st->cr4    = read_cr4();

    /* (a) Disable global pages so the upcoming TLB flush actually evicts
     *     entries that have the G bit set (we don't currently set G, but
     *     Intel mandates this step regardless). */
    if (st->cr4 & CR4_PGE)
        write_cr4(st->cr4 & ~CR4_PGE);

    /* (b) Disable caching (CD = 1) but keep write-back semantics (NW = 0).
     *     The combination CD=1/NW=0 is the only one Intel allows during
     *     MTRR reprogramming; CD=1/NW=1 would silently corrupt memory. */
    write_cr0((st->cr0 | CR0_CD) & ~CR0_NW);

    /* (c) Write back and invalidate every cache line so stale entries
     *     cannot survive with their old memory type. */
    cpu_wbinvd();

    /* (d) Flush the TLB (reload CR3 with itself). */
    write_cr3(read_cr3());

    /* (e) Disable MTRRs (clear master enable) while we mutate them. */
    st->def_type = rdmsr(IA32_MTRR_DEF_TYPE);
    wrmsr(IA32_MTRR_DEF_TYPE, st->def_type & ~(uint64_t)MTRR_DEF_TYPE_E);
}

static void mtrr_finish(const mtrr_prep_state_t* st) {
    /* Re-arm the master enable so the new variable ranges become active. */
    wrmsr(IA32_MTRR_DEF_TYPE, st->def_type | MTRR_DEF_TYPE_E);

    /* Drop any cache lines that may have been speculatively fetched under
     * the old memory type, and flush the TLB so PTE attributes are reread
     * the next time the affected pages are touched. */
    cpu_wbinvd();
    write_cr3(read_cr3());

    /* Re-enable caching. */
    write_cr0(st->cr0 & ~(CR0_CD | CR0_NW));

    /* Restore CR4.PGE if originally set. */
    if (st->cr4 & CR4_PGE)
        write_cr4(st->cr4);

    irq_restore(st->eflags);
}

/* ------------------------------------------------------------------------ *
 *  Variable MTRR slot management
 * ------------------------------------------------------------------------ */

static int mtrr_find_free_slot(void) {
    for (int n = 0; n < g_variable_count; n++) {
        uint64_t mask = rdmsr(IA32_MTRR_PHYSMASK(n));
        if (!(mask & MTRR_PHYSMASK_VALID))
            return n;
    }
    return -1;
}

static void mtrr_program_slot(int n, uint64_t base, uint64_t size,
                              uint8_t type)
{
    /* PhysBase: address bits [MAXPHYSADDR-1 : 12] + type in [7:0]. */
    uint64_t phys_base = (base & ~0xFFFull) | (uint64_t)type;

    /* PhysMask: bits to compare = ~(size-1), trimmed to MAXPHYSADDR,
     * plus the V (valid) bit at position 11. The high address bits
     * beyond our 32-bit kernel still need to be set so the comparator
     * does not pass on a 36/40-bit-wide PA the BIOS exposes. */
    uint64_t mask_lim = (g_maxphysaddr_bits >= 64)
                          ? ~0ull
                          : ((1ull << g_maxphysaddr_bits) - 1ull);
    uint64_t phys_mask = ((~(size - 1ull)) & mask_lim & ~0xFFFull)
                       | MTRR_PHYSMASK_VALID;

    /* Order matters per SDM: program PhysBase first, then PhysMask
     * (writing PhysMask is what arms the range via the V bit). */
    wrmsr(IA32_MTRR_PHYSBASE(n), phys_base);
    wrmsr(IA32_MTRR_PHYSMASK(n), phys_mask);
}

/* Greedy decomposition of [base, base+size) into naturally aligned
 * power-of-two chunks; programs each as a fresh variable MTRR slot.
 * Caller must have run mtrr_prepare() and provide a 4 KB-aligned base
 * and 4 KB-multiple size. */
static int mtrr_emit_chunks_greedy(uint32_t base, uint32_t size,
                                   uint8_t type)
{
    int used = 0;
    while (size > 0) {
        /* Largest power of two dividing the address. base is 4 KB-aligned
         * (caller invariant), so ctz(base) >= 12. */
        uint32_t align_log2 = (uint32_t)__builtin_ctz(base);

        /* Largest power of two not exceeding the remaining size. */
        uint32_t size_log2  = 31u - (uint32_t)__builtin_clz(size);

        uint32_t chunk_log2 = (align_log2 < size_log2) ? align_log2 : size_log2;
        uint32_t chunk_size = 1u << chunk_log2;

        int slot = mtrr_find_free_slot();
        if (slot < 0)
            return -3;

        mtrr_program_slot(slot, (uint64_t)base, (uint64_t)chunk_size, type);
        used++;

        base += chunk_size;
        size -= chunk_size;
    }
    return used;
}

/* ------------------------------------------------------------------------ *
 *  Public API
 * ------------------------------------------------------------------------ */

void mtrr_init(void) {
    /* CPUID leaf 1: EDX bit 12 = MTRR support. */
    uint32_t a, b, c, d;
    cpuid_raw(1, &a, &b, &c, &d);
    if (!(d & (1u << 12))) {
        kprint("[MTRR] CPU does not advertise MTRR (CPUID.01H:EDX.MTRR=0)\n");
        klog(LOG_WARN, "MTRR unavailable — framebuffer stays UC");
        return;
    }
    g_mtrr_present = 1;

    uint64_t cap = rdmsr(IA32_MTRRCAP);
    g_variable_count = (int)(cap & MTRRCAP_VCNT_MASK);
    g_wc_supported   = (cap & MTRRCAP_WC_BIT) ? 1 : 0;

    /* MAXPHYSADDR from CPUID 0x80000008 EAX[7:0]; otherwise stick with 36. */
    cpuid_raw(0x80000000, &a, &b, &c, &d);
    if (a >= 0x80000008u) {
        cpuid_raw(0x80000008u, &a, &b, &c, &d);
        uint32_t bits = a & 0xFFu;
        if (bits >= 32 && bits <= 52)
            g_maxphysaddr_bits = bits;
    }

    if (!g_wc_supported)
        klog(LOG_WARN, "MTRR present but WC type not advertised");

    klog(LOG_OK, "MTRR subsystem probed (variable ranges ready)");
}

int mtrr_wc_available(void) {
    return g_mtrr_present && g_wc_supported && g_variable_count > 0;
}

int mtrr_add_wc(uint32_t base, uint32_t size) {
    if (!mtrr_wc_available())     return -1;
    if (size == 0 || (base & 0xFFFu)) return -2;

    /* Round size up to a 4 KB multiple. */
    size = (size + 0xFFFu) & ~0xFFFu;

    mtrr_prep_state_t st;
    mtrr_prepare(&st);
    int rc = mtrr_emit_chunks_greedy(base, size, MTRR_TYPE_WC);
    mtrr_finish(&st);

    if (rc < 0)
        klog(LOG_WARN, "MTRR: not enough free variable ranges for WC region");
    return rc;
}

/* Walk the kernel PD and clear PCD|PWT on every PTE in [va, va+size).
 * Process PDs share these page tables for the upper half, so a single
 * pass is enough. INVLPG per page so the new attributes are visible to
 * the CPU's TLB immediately. */
static void mtrr_clear_pcd_pwt_range(uint32_t va, uint32_t size) {
    uint32_t end = va + size;
    for (uint32_t a = va; a < end; a += PAGE_SIZE) {
        uint32_t pdi = (a >> 22) & 0x3FF;
        uint32_t pti = (a >> 12) & 0x3FF;
        uint32_t pde = page_directory[pdi];
        if (!(pde & PAGE_PRESENT)) continue;
        uint32_t* pt = (uint32_t*)(pde & ~0xFFFu);
        if (!(pt[pti] & PAGE_PRESENT)) continue;
        pt[pti] &= ~(uint32_t)(PAGE_PCD | PAGE_PWT);
        cpu_invlpg(a);
    }
}

/* If `base` is naturally aligned to the next-power-of-two of `size`, return
 * that pow2; else 0. Lets us cover irregular FB sizes with a single MTRR. */
static uint32_t mtrr_single_pow2_cover(uint32_t base, uint32_t size) {
    if (size == 0) return 0;
    /* Next power of two >= size, with size already 4 KB-aligned. */
    uint32_t round_log2 = 32u - (uint32_t)__builtin_clz(size - 1u);
    if (round_log2 < 12u) round_log2 = 12u;
    if (round_log2 > 31u) return 0;
    uint32_t round = 1u << round_log2;
    return ((base & (round - 1u)) == 0) ? round : 0;
}

int mtrr_enable_wc_for_framebuffer(uint32_t fb_phys,
                                   uint32_t fb_pitch,
                                   uint32_t fb_height)
{
    if (!fb_phys || !fb_pitch || !fb_height)
        return -2;

    fb_phys &= ~0xFFFu;

    /* True footprint of the visible framebuffer, page-aligned. */
    uint32_t fb_size = fb_pitch * fb_height;
    fb_size = (fb_size + 0xFFFu) & ~0xFFFu;

    if (!mtrr_wc_available()) {
        klog(LOG_WARN, "framebuffer kept UC (no MTRR/WC) — rendering will be slow");
        return -1;
    }

    /* Strategy: prefer a single MTRR slot if the framebuffer's natural
     * alignment lets us round size up to a power of two and stay aligned.
     * This is the common QEMU stdvga / typical PCI BAR case and burns just
     * one of the (usually 8) variable ranges. Otherwise fall through to the
     * greedy decomposition. */
    int slots_used;
    mtrr_prep_state_t st;
    mtrr_prepare(&st);

    uint32_t single = mtrr_single_pow2_cover(fb_phys, fb_size);
    if (single != 0) {
        int slot = mtrr_find_free_slot();
        if (slot < 0) {
            mtrr_finish(&st);
            klog(LOG_WARN, "MTRR: no free variable range slot for FB");
            return -3;
        }
        mtrr_program_slot(slot, fb_phys, single, MTRR_TYPE_WC);
        slots_used = 1;
    } else {
        slots_used = mtrr_emit_chunks_greedy(fb_phys, fb_size, MTRR_TYPE_WC);
    }
    mtrr_finish(&st);

    if (slots_used < 0) {
        klog(LOG_WARN, "MTRR: framebuffer WC programming failed");
        return slots_used;
    }

    /* Drop PCD/PWT on the FB pages — without this, the effective memory
     * type stays UC regardless of what the MTRR says. */
    mtrr_clear_pcd_pwt_range(fb_phys, fb_size);

    return 0;
}
