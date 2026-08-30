#include "procfs.h"
#include "procfs_internal.h"
#include "vfs.h"
#include "kernel.h"
#include "memory.h"
#include "klib.h"
#include "task.h"
#include "version.h"
#include "pci_modblob.h"
#include "cpudev.h"
#include "apic.h"
#include "msi.h"

// Approximate CPU MHz via short TSC busy-wait.
static uint32_t _tsc_mhz(void) {
    uint32_t lo1, hi1, lo2, hi2;
    __asm__ volatile("rdtsc" : "=a"(lo1), "=d"(hi1));
    volatile uint32_t c = 10000000;
    while (c--) __asm__ volatile("nop");
    __asm__ volatile("rdtsc" : "=a"(lo2), "=d"(hi2));
    uint32_t delta = lo2 - lo1;
    return delta / 10000u;
}

// /proc/cpuinfo generator — uses cpudev.c cached data
int _cpuinfo_read(uint32_t off, uint32_t size, char *buf) {
    char tmp[768];
    int  p = 0;

    uint32_t mhz = _tsc_mhz();
    uint32_t apic_id = apic_lapic_id();

    #define _APP(s) { const char *_s=(s); while(*_s) tmp[p++]=*_s++; }
    #define _APPN(n) { char _nb[16]; snprintf(_nb, sizeof(_nb), "%d", (int)(n)); _APP(_nb); }

    _APP("processor       : 0\n");
    _APP("apicid          : "); _APPN(apic_id); _APP("\n");
    _APP("vendor_id       : "); _APP(cpu_vendor_str(cpu_vendor())); _APP("\n");
    _APP("model name      : "); _APP(cpu_brand_str()); _APP("\n");
    _APP("cpu MHz         : "); _APPN(mhz); _APP("\n");

    _APP("flags           :");
    uint32_t edx = cpu_features_edx();
    uint32_t ecx = cpu_features_ecx();
    uint32_t ext_edx = cpu_features_ext_edx();
    uint32_t l7_ebx = cpu_features_leaf7_ebx();
    uint32_t l7_ecx = cpu_features_leaf7_ecx();
    uint32_t l7_edx = cpu_features_leaf7_edx();

    if (edx & (1u<<0))  _APP(" fpu");
    if (edx & (1u<<1))  _APP(" vme");
    if (edx & (1u<<2))  _APP(" de");
    if (edx & (1u<<3))  _APP(" pse");
    if (edx & (1u<<4))  _APP(" tsc");
    if (edx & (1u<<5))  _APP(" msr");
    if (edx & (1u<<6))  _APP(" pae");
    if (edx & (1u<<7))  _APP(" mce");
    if (edx & (1u<<8))  _APP(" cx8");
    if (edx & (1u<<9))  _APP(" apic");
    if (edx & (1u<<11)) _APP(" sep");
    if (edx & (1u<<12)) _APP(" mtrr");
    if (edx & (1u<<13)) _APP(" pge");
    if (edx & (1u<<14)) _APP(" mca");
    if (edx & (1u<<15)) _APP(" cmov");
    if (edx & (1u<<16)) _APP(" pat");
    if (edx & (1u<<17)) _APP(" pse36");
    if (edx & (1u<<19)) _APP(" clflush");
    if (edx & (1u<<21)) _APP(" ds");
    if (edx & (1u<<22)) _APP(" acpi");
    if (edx & (1u<<23)) _APP(" mmx");
    if (edx & (1u<<24)) _APP(" fxsr");
    if (edx & (1u<<25)) _APP(" sse");
    if (edx & (1u<<26)) _APP(" sse2");
    if (edx & (1u<<28)) _APP(" ht");
    if (edx & (1u<<29)) _APP(" tm1");
    if (edx & (1u<<31)) _APP(" pbe");

    if (ecx & (1u<<0))  _APP(" sse3");
    if (ecx & (1u<<1))  _APP(" pclmulqdq");
    if (ecx & (1u<<2))  _APP(" dtes64");
    if (ecx & (1u<<3))  _APP(" monitor");
    if (ecx & (1u<<4))  _APP(" dscpl");
    if (ecx & (1u<<5))  _APP(" vmx");
    if (ecx & (1u<<6))  _APP(" smx");
    if (ecx & (1u<<7))  _APP(" est");
    if (ecx & (1u<<8))  _APP(" tm2");
    if (ecx & (1u<<9))  _APP(" ssse3");
    if (ecx & (1u<<12)) _APP(" fma");
    if (ecx & (1u<<13)) _APP(" cx16");
    if (ecx & (1u<<14)) _APP(" xtpr");
    if (ecx & (1u<<15)) _APP(" pdcm");
    if (ecx & (1u<<17)) _APP(" pcid");
    if (ecx & (1u<<19)) _APP(" sse4_1");
    if (ecx & (1u<<20)) _APP(" sse4_2");
    if (ecx & (1u<<21)) _APP(" x2apic");
    if (ecx & (1u<<22)) _APP(" movbe");
    if (ecx & (1u<<23)) _APP(" popcnt");
    if (ecx & (1u<<24)) _APP(" tsc-deadline");
    if (ecx & (1u<<25)) _APP(" aes");
    if (ecx & (1u<<26)) _APP(" xsave");
    if (ecx & (1u<<27)) _APP(" osxsave");
    if (ecx & (1u<<28)) _APP(" avx");
    if (ecx & (1u<<29)) _APP(" f16c");
    if (ecx & (1u<<30)) _APP(" rdrand");
    if (ecx & (1u<<31)) _APP(" hypervisor");

    if (l7_ebx & (1u<<0))  _APP(" fsgsbase");
    if (l7_ebx & (1u<<1))  _APP(" tsc-adjust");
    if (l7_ebx & (1u<<2))  _APP(" sgx");
    if (l7_ebx & (1u<<3))  _APP(" bmi1");
    if (l7_ebx & (1u<<4))  _APP(" hle");
    if (l7_ebx & (1u<<5))  _APP(" avx2");
    if (l7_ebx & (1u<<7))  _APP(" smep");
    if (l7_ebx & (1u<<8))  _APP(" bmi2");
    if (l7_ebx & (1u<<9))  _APP(" erms");
    if (l7_ebx & (1u<<10)) _APP(" invpcid");
    if (l7_ebx & (1u<<11)) _APP(" rtm");
    if (l7_ebx & (1u<<14)) _APP(" mpx");
    if (l7_ebx & (1u<<18)) _APP(" rdseed");
    if (l7_ebx & (1u<<19)) _APP(" adx");
    if (l7_ebx & (1u<<20)) _APP(" smap");
    if (l7_ebx & (1u<<23)) _APP(" clflushopt");
    if (l7_ebx & (1u<<24)) _APP(" clwb");
    if (l7_ebx & (1u<<29)) _APP(" sha");

    if (l7_ecx & (1u<<2))  _APP(" umip");
    if (l7_ecx & (1u<<3))  _APP(" pku");
    if (l7_ecx & (1u<<4))  _APP(" ospke");
    if (l7_ecx & (1u<<7))  _APP(" cet-ss");
    if (l7_ecx & (1u<<9))  _APP(" vaes");
    if (l7_ecx & (1u<<10)) _APP(" vpclmulqdq");
    if (l7_ecx & (1u<<16)) _APP(" la57");

    if (l7_edx & (1u<<10)) _APP(" md-clear");
    if (l7_edx & (1u<<26)) _APP(" ibrs");
    if (l7_edx & (1u<<27)) _APP(" stibp");
    if (l7_edx & (1u<<28)) _APP(" l1d-flush");
    if (l7_edx & (1u<<29)) _APP(" arch-cap");
    if (l7_edx & (1u<<30)) _APP(" core-cap");
    if (l7_edx & (1u<<31)) _APP(" ssbd");

    if (ext_edx & (1u<<11)) _APP(" syscall");
    if (ext_edx & (1u<<20)) _APP(" nx");
    if (ext_edx & (1u<<22)) _APP(" mmxext");
    if (ext_edx & (1u<<25)) _APP(" fxsr-opt");
    if (ext_edx & (1u<<26)) _APP(" pdpe1gb");
    if (ext_edx & (1u<<27)) _APP(" rdtscp");
    if (ext_edx & (1u<<29)) _APP(" lm");
    _APP("\n");

    #undef _APP
    #undef _APPN

    uint32_t len = (uint32_t)p;
    if (off >= len) return 0;
    if (size > len - off) size = len - off;
    memcpy(buf, tmp + off, size);
    return (int)size;
}

// Memory total (set by mntfs during init)
static uint32_t _mb_mem_total_kb = 0;

void procfs_set_meminfo(uint32_t mem_total_kb) {
    _mb_mem_total_kb = mem_total_kb;
}

static uint32_t _get_total_memory_kb(void) {
    return _mb_mem_total_kb;
}

// /proc/apic generator
int _apic_read(uint32_t off, uint32_t size, char *buf) {
    char tmp[512]; int p = 0;

    #define _A(s) { const char *_s=(s); while(*_s) tmp[p++]=*_s++; }
    #define _N(n) { char _b[16]; snprintf(_b, sizeof(_b), "%d", (int)(n)); _A(_b); }

    _A("APIC enabled    : ");
    _A(apic_is_enabled() ? "yes" : "no"); _A("\n");

    if (apic_is_enabled()) {
        _A("LAPIC base      : 0x");
        { char h[12]; snprintf(h, sizeof(h), "0x%x", (unsigned)apic_lapic_base()); _A(h); }
        _A("\n");
        _A("LAPIC ID        : "); _N(apic_lapic_id()); _A("\n");

        uint32_t io_base, io_id, io_max, io_gsi;
        if (apic_ioapic_info(&io_base, &io_id, &io_max, &io_gsi)) {
            _A("IOAPIC ID       : "); _N(io_id); _A("\n");
            _A("IOAPIC base     : 0x");
            { char h[12]; snprintf(h, sizeof(h), "0x%x", (unsigned)io_base); _A(h); }
            _A("\n");
            _A("IOAPIC max entry: "); _N(io_max); _A("\n");
            _A("IOAPIC GSI base : "); _N(io_gsi); _A("\n");

            _A("ISA IRQ overrides:");
            for (int i = 0; i < 16; i++) {
                int gsi = apic_irq_override(i);
                if (gsi != i) {
                    _A(" "); _N(i); _A("->"); _N(gsi);
                }
            }
            _A("\n");
        }
    }

    _A("MSI-X vectors   : 0x");
    { char h[12]; snprintf(h, sizeof(h), "0x%x", (unsigned)MSIX_VECTOR_BASE); _A(h); }
    _A("-0x");
    { char h[12]; snprintf(h, sizeof(h), "0x%x", (unsigned)(MSIX_VECTOR_END - 1)); _A(h); }
    _A(" ("); _N(MSIX_VECTOR_COUNT); _A(" total)\n");

    int used = msix_used_vectors();
    _A("MSI-X used      : "); _N(used); _A("\n");
    _A("MSI-X free      : "); _N(MSIX_VECTOR_COUNT - used); _A("\n");

    #undef _A
    #undef _N

    uint32_t len = (uint32_t)p;
    if (off >= len) return 0;
    if (size > len - off) size = len - off;
    memcpy(buf, tmp + off, size);
    return (int)size;
}

// /proc/meminfo generator
int _meminfo_read(uint32_t off, uint32_t size, char *buf) {
    char tmp[256]; int p = 0;

    unsigned int free_kb  = get_free_heap_memory() / 1024;
    unsigned int total_kb = _get_total_memory_kb();
    unsigned int used_kb  = total_kb > free_kb ? total_kb - free_kb : 0;

    #define _A(s) { const char *_s=(s); while(*_s) tmp[p++]=*_s++; }
    #define _N(n) { char _b[16]; snprintf(_b, sizeof(_b), "%d", (int)(n)); _A(_b); }

    _A("MemTotal:     "); _N(total_kb); _A(" kB\n");
    _A("MemFree:      "); _N(free_kb);  _A(" kB\n");
    _A("MemUsed:      "); _N(used_kb);  _A(" kB\n");
    _A("SwapTotal:    0 kB\n");
    _A("SwapFree:     0 kB\n");

    #undef _A
    #undef _N

    uint32_t len = (uint32_t)p;
    if (off >= len) return 0;
    if (size > len - off) size = len - off;
    memcpy(buf, tmp + off, size);
    return (int)size;
}

extern unsigned int timer_ticks_get(void);

// /proc/uptime generator
int _uptime_read(uint32_t off, uint32_t size, char *buf) {
    char tmp[64];
    int  p = 0;
    char nb[16]; snprintf(nb, sizeof(nb), "%d", (int)(timer_ticks_get() / 100));
    for (int i = 0; nb[i]; i++) tmp[p++] = nb[i];
    const char *s = " seconds\n";
    for (int i = 0; s[i]; i++) tmp[p++] = s[i];
    uint32_t len = (uint32_t)p;
    if (off >= len) return 0;
    if (size > len - off) size = len - off;
    memcpy(buf, tmp + off, size);
    return (int)size;
}

// /proc/version generator
int _version_read(uint32_t off, uint32_t size, char *buf) {
    char tmp[512];
    int  p = 0;

    #define _V(s) { const char *_s=(s); while(*_s) tmp[p++]=*_s++; }

    _V("Cact Kernel ");  _V(kernel_version);    _V("\n");
    _V("Arch: x86 (i686)\n");
    _V("Compiler: GCC\n");
    _V("Commit: ");     _V(kernel_commit_hash); _V("\n");
    _V("Build: ");      _V(kernel_build_time);  _V("\n");

    #undef _V

    uint32_t len = (uint32_t)p;
    if (off >= len) return 0;
    if (size > len - off) size = len - off;
    memcpy(buf, tmp + off, size);
    return (int)size;
}

// Snapshot entry for /proc/tasks
typedef struct { uint32_t pid; task_state state; uint8_t is_kernel; } task_snap_t;
#define TASKS_SNAP_MAX 64

// /proc/tasks generator
int _tasks_read(uint32_t off, uint32_t size, char *buf) {
    task_snap_t snap[TASKS_SNAP_MAX];
    int count = 0;

    irq_spinlock_acquire(&scheduler_lock);
    struct task_struct *head = (struct task_struct *)task_list_head;
    struct task_struct *t    = head;
    if (t) do {
        snap[count].pid       = t->pid;
        snap[count].state     = t->state;
        snap[count].is_kernel = t->is_kernel;
        count++;
        t = t->next;
    } while (t && t != head && count < TASKS_SNAP_MAX);
    irq_spinlock_release(&scheduler_lock);

    char tmp[2048];
    int  p = 0;

    #define _A(s) { const char *_s=(s); while(*_s && p<(int)sizeof(tmp)-1) tmp[p++]=*_s++; }
    #define _N(n) { char _b[16]; snprintf(_b, sizeof(_b), "%d", (int)(n)); _A(_b); }

    _A("PID  STATE     TYPE\n");
    _A("---  --------  --------\n");

    for (int i = 0; i < count; i++) {
        _N(snap[i].pid);
        int digits = snap[i].pid < 10 ? 1 : snap[i].pid < 100 ? 2 : snap[i].pid < 1000 ? 3 : 4;
        for (int j = digits; j < 5; j++) { if (p < (int)sizeof(tmp)-1) tmp[p++] = ' '; }

        switch (snap[i].state) {
            case TASK_READY:    _A("ready     "); break;
            case TASK_RUNNING:  _A("running   "); break;
            case TASK_SLEEPING: _A("sleeping  "); break;
            case TASK_ZOMBIE:   _A("zombie    "); break;
            default:            _A("unknown   "); break;
        }

        _A(snap[i].is_kernel ? "kernel\n" : "user\n");
    }

    #undef _A
    #undef _N

    uint32_t len = (uint32_t)p;
    if (off >= len) return 0;
    if (size > len - off) size = len - off;
    memcpy(buf, tmp + off, size);
    return (int)size;
}
