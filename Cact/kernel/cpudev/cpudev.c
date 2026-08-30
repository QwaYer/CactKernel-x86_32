#include "cpudev.h"
#include "kernel.h"

#define IA32_SYSENTER_CS    0x174
#define IA32_SYSENTER_ESP   0x175
#define IA32_SYSENTER_EIP   0x176

// AMD SYSCALL/SYSRET MSRs (extended — 0xC000_xxxx).
// On AMD-family CPUs these enable the legacy-mode fast syscall path that
// parallels SYSENTER/SYSEXIT on Intel.  Intel only supports SYSCALL in
// IA-32e (64-bit) mode, so the SYSCALL mechanism is selected only for
// AMD-family vendors (see cpu_vendor_is_amd_family()).
#define AMD_EFER            0xC0000080u
#define AMD_EFER_SCE        (1u << 0)   // SYSCALL/SYSRET enable
#define AMD_STAR            0xC0000081u
#define AMD_FMASK           0xC0000084u

static cpu_vendor_t   g_vendor             = CPU_VENDOR_UNKNOWN;
static syscall_mech_t g_syscall_mech       = SYSCALL_MECH_INT80;
static uint32_t       g_features_edx       = 0;
static uint32_t       g_features_ecx       = 0;
static uint32_t       g_features_ext_edx   = 0;
static uint32_t       g_features_leaf7_ebx = 0;
static uint32_t       g_features_leaf7_ecx = 0;
static uint32_t       g_features_leaf7_edx = 0;
static char           g_brand[49]          = {0};

// Return-path selector for sysenter_entry: 1 = SYSEXIT, 0 = IRET.
// Read by interrupt.asm on every syscall return so the two paths can be
// compared at runtime.
uint8_t g_syscall_use_sysexit = 1;

static inline void cpuid_raw(uint32_t leaf, uint32_t *eax, uint32_t *ebx,
                              uint32_t *ecx, uint32_t *edx) {
    __asm__ __volatile__("cpuid"
        : "=a"(*eax), "=b"(*ebx), "=c"(*ecx), "=d"(*edx)
        : "a"(leaf), "c"(0));
}

static inline void cpuid_raw_subleaf(uint32_t leaf, uint32_t subleaf,
                                      uint32_t *eax, uint32_t *ebx,
                                      uint32_t *ecx, uint32_t *edx) {
    __asm__ __volatile__("cpuid"
        : "=a"(*eax), "=b"(*ebx), "=c"(*ecx), "=d"(*edx)
        : "a"(leaf), "c"(subleaf));
}

static inline uint64_t rdmsr(uint32_t msr) {
    uint32_t lo, hi;
    __asm__ __volatile__("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}

static inline void wrmsr(uint32_t msr, uint64_t val) {
    uint32_t lo = (uint32_t)val, hi = (uint32_t)(val >> 32);
    __asm__ __volatile__("wrmsr" :: "c"(msr), "a"(lo), "d"(hi));
}

// Per-task update of IA32_SYSENTER_ESP. The sysenter instruction loads ESP
// from this MSR (NOT the TSS), so the scheduler must keep it in sync with the
// current task's kernel stack on every context switch — just like tss_entry.esp0.
void syscall_set_esp0(uint32_t esp) {
    wrmsr(IA32_SYSENTER_ESP, (uint64_t)esp);
}

uint8_t cpu_syscall_use_sysexit(void) { return g_syscall_use_sysexit; }
void   cpu_syscall_set_use_sysexit(uint8_t v) { g_syscall_use_sysexit = v ? 1 : 0; }

static int vendor_match(uint32_t ebx, uint32_t ecx, uint32_t edx,
                         const char* s) {
    const unsigned char* b = (const unsigned char*)&ebx;
    const unsigned char* c = (const unsigned char*)&ecx;
    const unsigned char* d = (const unsigned char*)&edx;
    for (int i = 0; i < 4; i++) {
        if (b[i] != (unsigned char)s[i])   return 0;
        if (c[i] != (unsigned char)s[i+4]) return 0;
        if (d[i] != (unsigned char)s[i+8]) return 0;
    }
    return 1;
}

static cpu_vendor_t detect_vendor(void) {
    uint32_t eax, ebx, ecx, edx;
    cpuid_raw(0, &eax, &ebx, &ecx, &edx);

    if (vendor_match(ebx, ecx, edx, "GenuineIntel"))
        return CPU_VENDOR_INTEL;
    if (vendor_match(ebx, ecx, edx, "AuthenticAMD"))
        return CPU_VENDOR_AMD;
    if (vendor_match(ebx, ecx, edx, "CentaurHauls"))
        return CPU_VENDOR_VIA;
    if (vendor_match(ebx, ecx, edx, "HygonGenuine"))
        return CPU_VENDOR_HYGON;
    if (vendor_match(ebx, ecx, edx, "  Shanghai  "))
        return CPU_VENDOR_ZHAOXIN;
    return CPU_VENDOR_UNKNOWN;
}

const char* cpu_vendor_str(cpu_vendor_t vendor) {
    switch (vendor) {
    case CPU_VENDOR_INTEL:   return "Intel";
    case CPU_VENDOR_AMD:     return "AMD";
    case CPU_VENDOR_VIA:     return "VIA";
    case CPU_VENDOR_HYGON:   return "Hygon";
    case CPU_VENDOR_ZHAOXIN: return "Zhaoxin";
    default:                 return "Unknown";
    }
}

const char* cpu_syscall_mech_str(syscall_mech_t mech) {
    switch (mech) {
    case SYSCALL_MECH_SYSENTER: return "SYSENTER/SYSEXIT";
    case SYSCALL_MECH_SYSCALL:  return "SYSCALL/SYSRET";
    default:                    return "int 0x80";
    }
}

// SYSCALL/SYSRET in 32-bit legacy mode is an AMD-defined feature: AMD, Hygon,
// VIA/Centaur and Zhaoxin implement it; Intel only supports SYSCALL inside
// IA-32e (64-bit) mode and raises #UD for it in legacy mode.  So the SYSCALL
// mechanism is only eligible on AMD-family vendors.
static int cpu_vendor_is_amd_family(void) {
    switch (g_vendor) {
    case CPU_VENDOR_AMD:
    case CPU_VENDOR_HYGON:
    case CPU_VENDOR_VIA:
    case CPU_VENDOR_ZHAOXIN:
        return 1;
    default:
        return 0;
    }
}

cpu_vendor_t   cpu_vendor(void)          { return g_vendor; }
syscall_mech_t cpu_syscall_mech(void)     { return g_syscall_mech; }
uint32_t       cpu_features_edx(void)     { return g_features_edx; }
uint32_t       cpu_features_ecx(void)     { return g_features_ecx; }
uint32_t       cpu_features_ext_edx(void) { return g_features_ext_edx; }
uint32_t       cpu_features_leaf7_ebx(void) { return g_features_leaf7_ebx; }
uint32_t       cpu_features_leaf7_ecx(void) { return g_features_leaf7_ecx; }
uint32_t       cpu_features_leaf7_edx(void) { return g_features_leaf7_edx; }

int cpu_has_sep(void)    { return !!(g_features_edx & CPU_FEATURE_SEP); }
int cpu_has_syscall(void){ return !!(g_features_ext_edx & CPU_FEATURE_SYSCALL); }
int cpu_has_pat(void)    { return !!(g_features_edx & CPU_FEATURE_PAT); }
int cpu_has_msr(void)    { return !!(g_features_edx & CPU_FEATURE_MSR); }
int cpu_has_fpu(void)    { return !!(g_features_edx & CPU_FEATURE_FPU); }
int cpu_has_sse(void)    { return !!(g_features_edx & CPU_FEATURE_SSE); }
int cpu_has_sse2(void)   { return !!(g_features_edx & CPU_FEATURE_SSE2); }
int cpu_has_sse3(void)   { return !!(g_features_ecx & CPU_FEATURE_SSE3); }
int cpu_has_ssse3(void)  { return !!(g_features_ecx & CPU_FEATURE_SSSE3); }
int cpu_has_sse41(void)  { return !!(g_features_ecx & CPU_FEATURE_SSE41); }
int cpu_has_sse42(void)  { return !!(g_features_ecx & CPU_FEATURE_SSE42); }
int cpu_has_avx(void)    { return !!(g_features_ecx & CPU_FEATURE_AVX); }
int cpu_has_avx2(void)   { return !!(g_features_leaf7_ebx & CPU_FEATURE_AVX2); }
int cpu_has_fma(void)    { return !!(g_features_ecx & CPU_FEATURE_FMA); }
int cpu_has_aes(void)    { return !!(g_features_ecx & CPU_FEATURE_AES); }
int cpu_has_vmx(void)    { return !!(g_features_ecx & CPU_FEATURE_VMX); }
int cpu_has_smep(void)   { return !!(g_features_leaf7_ebx & CPU_FEATURE_SMEP); }
int cpu_has_smap(void)   { return !!(g_features_leaf7_ebx & CPU_FEATURE_SMAP); }
int cpu_has_umip(void)   { return !!(g_features_leaf7_ecx & CPU_FEATURE_UMIP); }
int cpu_has_pku(void)    { return !!(g_features_leaf7_ecx & CPU_FEATURE_PKU); }
int cpu_has_ibrs(void)   { return !!(g_features_leaf7_edx & CPU_FEATURE_IBRS_IBPB); }
int cpu_has_stibp(void)  { return !!(g_features_leaf7_edx & CPU_FEATURE_STIBP); }
int cpu_has_ssbd(void)   { return !!(g_features_leaf7_edx & CPU_FEATURE_SSBD); }
int cpu_has_md_clear(void) { return !!(g_features_leaf7_edx & CPU_FEATURE_MD_CLEAR); }
int cpu_has_mtrr(void)   { return !!(g_features_edx & CPU_FEATURE_MTRR); }
int cpu_has_pge(void)    { return !!(g_features_edx & CPU_FEATURE_PGE); }
int cpu_has_pae(void)    { return !!(g_features_edx & CPU_FEATURE_PAE); }
int cpu_has_apic(void)   { return !!(g_features_edx & CPU_FEATURE_APIC); }
int cpu_has_x2apic(void) { return !!(g_features_ecx & CPU_FEATURE_X2APIC); }
int cpu_has_htt(void)    { return !!(g_features_edx & CPU_FEATURE_HTT); }
int cpu_has_nx(void)     { return !!(g_features_ext_edx & CPU_FEATURE_NX); }
int cpu_has_gbpages(void){ return !!(g_features_ext_edx & CPU_FEATURE_GBPAGES); }
int cpu_has_rdtscp(void) { return !!(g_features_ext_edx & CPU_FEATURE_RDTSCP); }
int cpu_has_invpcid(void){ return !!(g_features_leaf7_ebx & CPU_FEATURE_INVPCID); }
int cpu_has_rdrand(void) { return !!(g_features_ecx & CPU_FEATURE_RDRAND); }
int cpu_has_arat(void) {
    uint32_t eax;
    cpuid_raw(6, &eax, &(uint32_t){0}, &(uint32_t){0}, &(uint32_t){0});
    return !!(eax & CPU_FEATURE_ARAT);
}
int cpu_has_hypervisor(void) { return !!(g_features_ecx & CPU_FEATURE_HYPERVISOR); }

const char* cpu_brand_str(void) { return g_brand; }

static void read_brand(void) {
    union {
        char     c[48];
        uint32_t u[12];
    } brand;
    uint32_t eax, ebx, ecx, edx;
    uint32_t leaf;
    for (leaf = 0x80000002; leaf <= 0x80000004; leaf++) {
        cpuid_raw(leaf, &eax, &ebx, &ecx, &edx);
        int i = (int)(leaf - 0x80000002) * 4;
        brand.u[i + 0] = eax;
        brand.u[i + 1] = ebx;
        brand.u[i + 2] = ecx;
        brand.u[i + 3] = edx;
    }
    brand.c[47] = '\0';
    const char *p = brand.c;
    while (*p == ' ') p++;
    for (int i = 0; i < 48 && p[i] != '\0'; i++)
        g_brand[i] = p[i];
    g_brand[47] = '\0';
}

static void detect_leaf7(void) {
    uint32_t max_leaf;
    cpuid_raw(0, &max_leaf, &(uint32_t){0}, &(uint32_t){0}, &(uint32_t){0});
    if (max_leaf < 7)
        return;
    cpuid_raw_subleaf(7, 0, &(uint32_t){0},
                       &g_features_leaf7_ebx,
                       &g_features_leaf7_ecx,
                       &g_features_leaf7_edx);
}

int cpudev_init(void) {
    g_vendor = detect_vendor();

    cpuid_raw(1,
              &(uint32_t){0}, &(uint32_t){0},
              &g_features_ecx, &g_features_edx);

    uint32_t max_ext;
    cpuid_raw(0x80000000, &max_ext,
              &(uint32_t){0}, &(uint32_t){0}, &(uint32_t){0});
    if (max_ext >= 0x80000001) {
        cpuid_raw(0x80000001,
                  &(uint32_t){0}, &(uint32_t){0},
                  &(uint32_t){0}, &g_features_ext_edx);
    }

    if (max_ext >= 0x80000004)
        read_brand();

    detect_leaf7();

    printk("CPUDEV: ");
    printk((char*)cpu_vendor_str(g_vendor));
    if (g_brand[0]) {
        printk(" ");
        printk((char*)cpu_brand_str());
    }
    printk("\n");

    // Choose the fastest syscall mechanism this CPU can do in 32-bit mode.
    // AMD-family CPUs with the SYSCALL bit use SYSCALL/SYSRET (native AMD fast
    // path).  Everything else with SEP falls back to SYSENTER/SYSEXIT.  The
    // legacy int 0x80 path has been removed, so one of the two is mandatory.
    if (cpu_vendor_is_amd_family() && cpu_has_syscall()) {
        g_syscall_mech = SYSCALL_MECH_SYSCALL;
    } else if (cpu_has_sep()) {
        g_syscall_mech = SYSCALL_MECH_SYSENTER;
    } else {
        g_syscall_mech = SYSCALL_MECH_SYSENTER;  // placeholder
        pr_crit("CPUDEV: CPU supports neither SYSCALL nor SEP — syscalls will NOT work");
    }

    printk("CPUDEV: syscall = ");
    printk((char*)cpu_syscall_mech_str(g_syscall_mech));
    printk("\n");

    return 0;
}

int cpu_syscall_commit(void) {
    extern void sysenter_entry(void);
    extern void syscall_entry(void);
    extern uint8_t early_kernel_stack[4096];
    uint32_t boot_esp = (uint32_t)(uintptr_t)
        (early_kernel_stack + sizeof(early_kernel_stack));

    // SYSENTER/SYSEXIT is always wired when the CPU has SEP, even on AMD, so
    // both entry paths stay functional.  The scheduler keeps
    // IA32_SYSENTER_ESP and tss_entry.esp0 in sync per context switch.
    if (cpu_has_sep()) {
        wrmsr(IA32_SYSENTER_CS,  0x08);
        wrmsr(IA32_SYSENTER_EIP, (uint64_t)(uintptr_t)sysenter_entry);
        wrmsr(IA32_SYSENTER_ESP, (uint64_t)boot_esp);
    }

    // AMD SYSCALL/SYSRET: enable via EFER.SCE, then point STAR at syscall_entry.
    //   STAR[31:0]   = legacy-mode SYSCALL target EIP
    //   STAR[47:32]  = SYSCALL CS base (kernel code 0x08; SS = +8 = 0x10)
    //   STAR[63:48]  = SYSRET  CS base (user   code 0x18; SS = +8 = 0x20→0x23)
    // FMASK = 0x200 clears IF on entry (the stub switches stacks off the user
    // stack, so interrupts must stay masked until the kernel stack is loaded).
    if (g_syscall_mech == SYSCALL_MECH_SYSCALL) {
        uint64_t efer = rdmsr(AMD_EFER);
        wrmsr(AMD_EFER, efer | AMD_EFER_SCE);

        uint64_t star = ((uint64_t)0x18 << 48) |
                        ((uint64_t)0x08 << 32) |
                        ((uint32_t)(uintptr_t)syscall_entry);
        wrmsr(AMD_STAR,  star);
        wrmsr(AMD_FMASK, 0x200);
    }

}
