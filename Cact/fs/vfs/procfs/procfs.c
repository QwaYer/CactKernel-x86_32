#include "procfs.h"
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

#define MDLS_MAX_FILES  32

// A read-only virtual file (e.g. cpuinfo, meminfo)
typedef struct proc_file {
    char            name[64];
    procfs_read_fn  read_fn;      // generates file content on demand
    vfs_node_t      node;
    struct proc_file *next;
} proc_file_t;

static proc_file_t *file_list = 0;

static vfs_node_t procfs_root;
static vfs_node_t mdls_dir;
static int        procfs_ready = 0;

// File read: delegate to the registered read_fn
static int _file_read(vfs_node_t *node, uint32_t off, uint32_t size, char *buf) {
    proc_file_t *f = (proc_file_t *)node->priv;
    if (!f || !f->read_fn) return 0;
    return f->read_fn(off, size, buf);
}

static vfs_ops_t file_ops = { .read = _file_read };

// mdls/ is a read-only view of bundled modules.
static vfs_node_t   mdls_files[MDLS_MAX_FILES];
static int          mdls_initialised = 0;
static vfs_dirent_t _mdls_de;

static const char *_mdls_basename(const char *path) {
    const char *last = path;
    for (const char *s = path; *s; s++)
        if (*s == '/') last = s + 1;
    return last;
}

static int _mdls_file_read(vfs_node_t *node, uint32_t off, uint32_t size,
                           char *buf) {
    int idx = (int)(uintptr_t)node->priv;
    const char    *path;
    const uint8_t *data;
    uint32_t       total;
    if (pci_modblob_at(idx, &path, &data, &total) != 0) return 0;
    if (off >= total) return 0;
    uint32_t avail = total - off;
    if (size > avail) size = avail;
    memcpy(buf, data + off, size);
    return (int)size;
}

static vfs_ops_t mdls_file_ops = { .read = _mdls_file_read };

static int _mdls_count(void) {
    int n = pci_modblob_count();
    if (n < 0) return 0;
    if (n > MDLS_MAX_FILES) n = MDLS_MAX_FILES;
    return n;
}

static void _mdls_init_lazy(void) {
    if (mdls_initialised) return;
    mdls_initialised = 1;

    int n = _mdls_count();
    for (int i = 0; i < n; i++) {
        const char    *path;
        const uint8_t *data;
        uint32_t       sz;
        if (pci_modblob_at(i, &path, &data, &sz) != 0) continue;
        const char *bn = _mdls_basename(path);
        memset(&mdls_files[i], 0, sizeof(vfs_node_t));
        strlcpy(mdls_files[i].name, bn, 128);
        mdls_files[i].type = VFS_FILE;
        mdls_files[i].ops  = &mdls_file_ops;
        mdls_files[i].priv = (void *)(uintptr_t)i;
    }
}

static vfs_node_t *_mdls_dir_walk(vfs_node_t *dir, const char *name) {
    (void)dir;
    _mdls_init_lazy();
    int n = _mdls_count();
    for (int i = 0; i < n; i++) {
        if (streq(mdls_files[i].name, name))
            return &mdls_files[i];
    }
    return 0;
}

static vfs_dirent_t *_mdls_dir_readdir(vfs_node_t *dir, uint32_t index) {
    (void)dir;
    _mdls_init_lazy();
    int n = _mdls_count();
    if ((int)index >= n) return 0;
    strlcpy(_mdls_de.name, mdls_files[index].name, 128);
    _mdls_de.inode = index + 1;
    return &_mdls_de;
}

static void _mdls_dir_listdir(vfs_node_t *dir) {
    (void)dir;
    _mdls_init_lazy();
    int n = _mdls_count();
    for (int i = 0; i < n; i++) {
        kprint("  "); kprint(mdls_files[i].name); kprint("\n");
    }
}

static vfs_ops_t mdls_dir_ops = {
    .walk    = _mdls_dir_walk,
    .readdir = _mdls_dir_readdir,
    .listdir = _mdls_dir_listdir,
};

// procfs root directory ops (mdls/ + virtual files)
static vfs_node_t *_root_walk(vfs_node_t *dir, const char *name) {
    (void)dir;
    if (streq(name, "mdls")) return &mdls_dir;

    for (proc_file_t *f = file_list; f; f = f->next)
        if (streq(f->name, name)) return &f->node;
    return 0;
}

static vfs_dirent_t _root_de;

static vfs_dirent_t *_root_readdir(vfs_node_t *dir, uint32_t index) {
    (void)dir;
    if (index == 0) {
        strlcpy(_root_de.name, "mdls", 128);
        _root_de.inode = 0;
        return &_root_de;
    }
    uint32_t i = 1;
    for (proc_file_t *f = file_list; f; f = f->next) {
        if (i++ == index) {
            strlcpy(_root_de.name, f->name, 128);
            _root_de.inode = i;
            return &_root_de;
        }
    }
    return 0;
}

static void _root_listdir(vfs_node_t *dir) {
    (void)dir;
    kprint("  mdls/\n");
    for (proc_file_t *f = file_list; f; f = f->next) {
        kprint("  "); kprint(f->name); kprint("\n");
    }
}

static vfs_ops_t root_ops = {
    .walk    = _root_walk,
    .readdir = _root_readdir,
    .listdir = _root_listdir,
};

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
static int _cpuinfo_read(uint32_t off, uint32_t size, char *buf) {
    char tmp[768];
    int  p = 0;

    uint32_t mhz = _tsc_mhz();
    uint32_t apic_id = apic_lapic_id();

    #define _APP(s) { const char *_s=(s); while(*_s) tmp[p++]=*_s++; }
    #define _APPN(n) { char _nb[16]; itoa((int)(n),_nb); _APP(_nb); }

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
static int _apic_read(uint32_t off, uint32_t size, char *buf) {
    char tmp[512]; int p = 0;

    #define _A(s) { const char *_s=(s); while(*_s) tmp[p++]=*_s++; }
    #define _N(n) { char _b[16]; itoa((int)(n),_b); _A(_b); }

    _A("APIC enabled    : ");
    _A(apic_is_enabled() ? "yes" : "no"); _A("\n");

    if (apic_is_enabled()) {
        _A("LAPIC base      : 0x");
        { char h[12]; hex_to_ascii(apic_lapic_base(), h); _A(h); }
        _A("\n");
        _A("LAPIC ID        : "); _N(apic_lapic_id()); _A("\n");

        uint32_t io_base, io_id, io_max, io_gsi;
        if (apic_ioapic_info(&io_base, &io_id, &io_max, &io_gsi)) {
            _A("IOAPIC ID       : "); _N(io_id); _A("\n");
            _A("IOAPIC base     : 0x");
            { char h[12]; hex_to_ascii(io_base, h); _A(h); }
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
    { char h[12]; hex_to_ascii(MSIX_VECTOR_BASE, h); _A(h); }
    _A("-0x");
    { char h[12]; hex_to_ascii(MSIX_VECTOR_END - 1, h); _A(h); }
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
static int _meminfo_read(uint32_t off, uint32_t size, char *buf) {
    char tmp[256]; int p = 0;

    unsigned int free_kb  = get_free_heap_memory() / 1024;
    unsigned int total_kb = _get_total_memory_kb();
    unsigned int used_kb  = total_kb > free_kb ? total_kb - free_kb : 0;

    #define _A(s) { const char *_s=(s); while(*_s) tmp[p++]=*_s++; }
    #define _N(n) { char _b[16]; itoa((int)(n),_b); _A(_b); }

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
static int _uptime_read(uint32_t off, uint32_t size, char *buf) {
    char tmp[64];
    int  p = 0;
    char nb[16]; itoa((int)(timer_ticks_get() / 100), nb);
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
static int _version_read(uint32_t off, uint32_t size, char *buf) {
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
static int _tasks_read(uint32_t off, uint32_t size, char *buf) {
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
    #define _N(n) { char _b[16]; itoa((int)(n),_b); _A(_b); }

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

// Return the procfs root VFS node (to be registered in mount table)
vfs_node_t *procfs_get_root(void) { return &procfs_root; }

// Register a read-only virtual file under /proc
int procfs_register_file(const char *name, procfs_read_fn read_fn) {
    if (!name) return -1;
    for (proc_file_t *f = file_list; f; f = f->next)
        if (streq(f->name, name)) return -1;

    proc_file_t *f = (proc_file_t *)kmalloc(sizeof(proc_file_t));
    if (!f) return -1;
    memset(f, 0, sizeof(proc_file_t));

    strlcpy(f->name, name, 64);
    f->read_fn = read_fn;

    memset(&f->node, 0, sizeof(vfs_node_t));
    strlcpy(f->node.name, name, 128);
    f->node.type = VFS_FILE;
    f->node.ops  = &file_ops;
    f->node.priv = f;

    f->next   = file_list;
    file_list = f;
    return 0;
}

// Unregister a virtual file
int procfs_unregister_file(const char *name) {
    proc_file_t **pp = &file_list;
    while (*pp) {
        if (streq((*pp)->name, name)) {
            proc_file_t *dead = *pp;
            *pp = dead->next;
            dead->read_fn = NULL;
            dead->node.priv = NULL;
            kfree_heap(dead);
            return 0;
        }
        pp = &(*pp)->next;
    }
    return -1;
}

// Initialize procfs root, subdirs, and default files.
void procfs_init(void) {
    if (procfs_ready) return;
    memset(&mdls_dir, 0, sizeof(vfs_node_t));
    strlcpy(mdls_dir.name, "mdls", 128);
    mdls_dir.type = VFS_DIRECTORY;
    mdls_dir.ops  = &mdls_dir_ops;

    memset(&procfs_root, 0, sizeof(vfs_node_t));
    strlcpy(procfs_root.name, "proc", 128);
    procfs_root.type = VFS_DIRECTORY;
    procfs_root.ops  = &root_ops;

    procfs_register_file("cpuinfo", _cpuinfo_read);
    procfs_register_file("apic",    _apic_read);
    procfs_register_file("meminfo", _meminfo_read);
    procfs_register_file("uptime",  _uptime_read);
    procfs_register_file("version", _version_read);
    procfs_register_file("tasks",   _tasks_read);

    procfs_ready = 1;
}