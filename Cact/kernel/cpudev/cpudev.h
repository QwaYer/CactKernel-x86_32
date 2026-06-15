#ifndef CPUDEV_H
#define CPUDEV_H

#include <stdint.h>

// ============================================================================
// CPU vendor identifiers
// ============================================================================
typedef enum {
    CPU_VENDOR_UNKNOWN = 0,
    CPU_VENDOR_INTEL,
    CPU_VENDOR_AMD,
    CPU_VENDOR_VIA,
    CPU_VENDOR_HYGON,
    CPU_VENDOR_ZHAOXIN,
} cpu_vendor_t;

// ============================================================================
// CPUID leaf 1, EDX — standard feature flags
// ============================================================================
#define CPU_FEATURE_FPU      (1u << 0)
#define CPU_FEATURE_VME      (1u << 1)
#define CPU_FEATURE_DE       (1u << 2)
#define CPU_FEATURE_PSE      (1u << 3)
#define CPU_FEATURE_TSC      (1u << 4)
#define CPU_FEATURE_MSR      (1u << 5)
#define CPU_FEATURE_PAE      (1u << 6)
#define CPU_FEATURE_MCE      (1u << 7)
#define CPU_FEATURE_CX8      (1u << 8)
#define CPU_FEATURE_APIC     (1u << 9)
#define CPU_FEATURE_SEP      (1u << 11)
#define CPU_FEATURE_MTRR     (1u << 12)
#define CPU_FEATURE_PGE      (1u << 13)
#define CPU_FEATURE_MCA      (1u << 14)
#define CPU_FEATURE_CMOV     (1u << 15)
#define CPU_FEATURE_PAT      (1u << 16)
#define CPU_FEATURE_PSE36    (1u << 17)
#define CPU_FEATURE_CLFSH    (1u << 19)
#define CPU_FEATURE_DS       (1u << 21)
#define CPU_FEATURE_ACPI     (1u << 22)
#define CPU_FEATURE_MMX      (1u << 23)
#define CPU_FEATURE_FXSR     (1u << 24)
#define CPU_FEATURE_SSE      (1u << 25)
#define CPU_FEATURE_SSE2     (1u << 26)
#define CPU_FEATURE_HTT      (1u << 28)
#define CPU_FEATURE_TM1      (1u << 29)
#define CPU_FEATURE_IA64     (1u << 30)
#define CPU_FEATURE_PBE      (1u << 31)

// ============================================================================
// CPUID leaf 1, ECX — standard feature flags
// ============================================================================
#define CPU_FEATURE_SSE3       (1u << 0)
#define CPU_FEATURE_PCLMULQDQ  (1u << 1)
#define CPU_FEATURE_DTES64     (1u << 2)
#define CPU_FEATURE_MONITOR    (1u << 3)
#define CPU_FEATURE_DSCPL      (1u << 4)
#define CPU_FEATURE_VMX        (1u << 5)
#define CPU_FEATURE_SMX        (1u << 6)
#define CPU_FEATURE_EST        (1u << 7)
#define CPU_FEATURE_TM2        (1u << 8)
#define CPU_FEATURE_SSSE3      (1u << 9)
#define CPU_FEATURE_FMA        (1u << 12)
#define CPU_FEATURE_CX16       (1u << 13)
#define CPU_FEATURE_XTPR       (1u << 14)
#define CPU_FEATURE_PDCM       (1u << 15)
#define CPU_FEATURE_PCID       (1u << 17)
#define CPU_FEATURE_DCA        (1u << 18)
#define CPU_FEATURE_SSE41      (1u << 19)
#define CPU_FEATURE_SSE42      (1u << 20)
#define CPU_FEATURE_X2APIC     (1u << 21)
#define CPU_FEATURE_MOVBE      (1u << 22)
#define CPU_FEATURE_POPCNT     (1u << 23)
#define CPU_FEATURE_TSC_D      (1u << 24)
#define CPU_FEATURE_AES        (1u << 25)
#define CPU_FEATURE_XSAVE      (1u << 26)
#define CPU_FEATURE_OSXSAVE    (1u << 27)
#define CPU_FEATURE_AVX        (1u << 28)
#define CPU_FEATURE_F16C       (1u << 29)
#define CPU_FEATURE_RDRAND     (1u << 30)
#define CPU_FEATURE_HYPERVISOR (1u << 31)

// ============================================================================
// CPUID leaf 7, subleaf 0 — structured extended features
// ============================================================================
// --- EBX ---
#define CPU_FEATURE_FSGSBASE   (1u << 0)
#define CPU_FEATURE_TSC_ADJUST (1u << 1)
#define CPU_FEATURE_SGX        (1u << 2)
#define CPU_FEATURE_BMI1       (1u << 3)
#define CPU_FEATURE_HLE        (1u << 4)
#define CPU_FEATURE_AVX2       (1u << 5)
#define CPU_FEATURE_SMEP       (1u << 7)
#define CPU_FEATURE_BMI2       (1u << 8)
#define CPU_FEATURE_ERMS       (1u << 9)
#define CPU_FEATURE_INVPCID    (1u << 10)
#define CPU_FEATURE_RTM        (1u << 11)
#define CPU_FEATURE_MPX        (1u << 14)
#define CPU_FEATURE_RDSEED     (1u << 18)
#define CPU_FEATURE_ADX        (1u << 19)
#define CPU_FEATURE_SMAP       (1u << 20)
#define CPU_FEATURE_CLFLUSHOPT (1u << 23)
#define CPU_FEATURE_CLWB       (1u << 24)
#define CPU_FEATURE_SHA        (1u << 29)

// --- ECX ---
#define CPU_FEATURE_UMIP       (1u << 2)
#define CPU_FEATURE_PKU        (1u << 3)
#define CPU_FEATURE_OSPKE      (1u << 4)
#define CPU_FEATURE_CET_SS     (1u << 7)
#define CPU_FEATURE_VAES       (1u << 9)
#define CPU_FEATURE_VPCLMULQDQ (1u << 10)
#define CPU_FEATURE_LA57       (1u << 16)

// --- EDX ---
#define CPU_FEATURE_MD_CLEAR   (1u << 10)
#define CPU_FEATURE_IBRS_IBPB  (1u << 26)
#define CPU_FEATURE_STIBP      (1u << 27)
#define CPU_FEATURE_L1D_FLUSH  (1u << 28)
#define CPU_FEATURE_ARCH_CAP   (1u << 29)
#define CPU_FEATURE_CORE_CAP   (1u << 30)
#define CPU_FEATURE_SSBD       (1u << 31)

// ============================================================================
// CPUID leaf 6, EAX — thermal / power management
// ============================================================================
#define CPU_FEATURE_DTS         (1u << 0)
#define CPU_FEATURE_TURBO       (1u << 1)
#define CPU_FEATURE_ARAT        (1u << 2)
#define CPU_FEATURE_PLN         (1u << 4)
#define CPU_FEATURE_ECMD        (1u << 5)
#define CPU_FEATURE_PTM         (1u << 6)

// ============================================================================
// CPUID leaf 0x80000001, EDX — extended features
// ============================================================================
#define CPU_FEATURE_SYSCALL     (1u << 11)
#define CPU_FEATURE_NX          (1u << 20)
#define CPU_FEATURE_MMXEXT      (1u << 22)
#define CPU_FEATURE_FFXSR       (1u << 25)
#define CPU_FEATURE_GBPAGES     (1u << 26)
#define CPU_FEATURE_RDTSCP      (1u << 27)
#define CPU_FEATURE_LM          (1u << 29)
#define CPU_FEATURE_3DNOWEXT    (1u << 30)
#define CPU_FEATURE_3DNOW       (1u << 31)

// ============================================================================
// Syscall mechanism
// ============================================================================
typedef enum {
    SYSCALL_MECH_INT80 = 0,
    SYSCALL_MECH_SYSENTER,
} syscall_mech_t;

// ============================================================================
// Public API
// ============================================================================
const char* cpu_vendor_str(cpu_vendor_t vendor);
const char* cpu_syscall_mech_str(syscall_mech_t mech);

int cpudev_init(void);

cpu_vendor_t    cpu_vendor(void);
syscall_mech_t  cpu_syscall_mech(void);

uint32_t cpu_features_edx(void);
uint32_t cpu_features_ecx(void);
uint32_t cpu_features_ext_edx(void);
uint32_t cpu_features_leaf7_ebx(void);
uint32_t cpu_features_leaf7_ecx(void);
uint32_t cpu_features_leaf7_edx(void);

int cpu_has_sep(void);
int cpu_has_syscall(void);
int cpu_has_pat(void);
int cpu_has_msr(void);
int cpu_has_fpu(void);
int cpu_has_sse(void);
int cpu_has_sse2(void);
int cpu_has_sse3(void);
int cpu_has_ssse3(void);
int cpu_has_sse41(void);
int cpu_has_sse42(void);
int cpu_has_avx(void);
int cpu_has_avx2(void);
int cpu_has_fma(void);
int cpu_has_aes(void);
int cpu_has_vmx(void);
int cpu_has_smep(void);
int cpu_has_smap(void);
int cpu_has_umip(void);
int cpu_has_pku(void);
int cpu_has_ibrs(void);
int cpu_has_stibp(void);
int cpu_has_ssbd(void);
int cpu_has_md_clear(void);
int cpu_has_mtrr(void);
int cpu_has_pge(void);
int cpu_has_pae(void);
int cpu_has_apic(void);
int cpu_has_x2apic(void);
int cpu_has_htt(void);
int cpu_has_nx(void);
int cpu_has_gbpages(void);
int cpu_has_rdtscp(void);
int cpu_has_invpcid(void);
int cpu_has_rdrand(void);
int cpu_has_arat(void);
int cpu_has_hypervisor(void);

const char* cpu_brand_str(void);

int cpu_syscall_commit(void);

#endif
