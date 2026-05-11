#include "procfs.h"
#include "vfs.h"
#include "kernel.h"
#include "memory.h"
#include "klib.h"
#include "task.h"
#include "version.h"
#include "pci_modblob.h"

/* /proc/bin/mdls exposes cctkfs modules by basename. */
#define MDLS_MAX_FILES  32

// A read-only virtual file (e.g. cpuinfo, meminfo)
typedef struct proc_file {
    char            name[64];
    procfs_read_fn  read_fn;      // generates file content on demand
    vfs_node_t      node;
    struct proc_file *next;
} proc_file_t;

// A read/write command node under /proc/cmd/
typedef struct proc_cmd {
    char            name[64];
    procfs_read_fn  help_fn;      // read returns help text (NULL → default)
    procfs_cmd_fn   cmd_fn;       // write invokes the command
    vfs_node_t      node;
    struct proc_cmd *next;
} proc_cmd_t;

// Global lists
static proc_file_t *file_list = 0;
static proc_cmd_t  *cmd_list  = 0;

static vfs_node_t procfs_root;
static vfs_node_t cmd_dir;
static vfs_node_t bin_dir;
static vfs_node_t mdls_dir;
static int        procfs_ready = 0;

// File read: delegate to the registered read_fn
static int _file_read(vfs_node_t *node, uint32_t off, uint32_t size, char *buf) {
    proc_file_t *f = (proc_file_t *)node->priv;
    if (!f || !f->read_fn) return 0;
    return f->read_fn(off, size, buf);
}

static vfs_ops_t file_ops = { .read = _file_read };

// Command read: return help text (or default "cmd: <name>")
static int _cmd_read(vfs_node_t *node, uint32_t off, uint32_t size, char *buf) {
    proc_cmd_t *c = (proc_cmd_t *)node->priv;
    if (!c) return 0;

    char tmp[256];
    int  n;
    if (c->help_fn) {
        n = c->help_fn(0, sizeof(tmp) - 1, tmp);
    } else {
        int p = 0;
        const char *pfx = "cmd: ";
        for (int i = 0; pfx[i]; i++) tmp[p++] = pfx[i];
        for (int i = 0; c->name[i]; i++) tmp[p++] = c->name[i];
        tmp[p++] = '\n'; tmp[p] = '\0';
        n = p;
    }

    if (n <= 0) return 0;
    if (off >= (uint32_t)n) return 0;
    uint32_t avail = (uint32_t)n - off;
    if (size > avail) size = avail;
    memcpy(buf, tmp + off, size);
    return (int)size;
}

// Command write: invoke the registered cmd_fn
static int _cmd_write(vfs_node_t *node, uint32_t off, uint32_t size, char *buf) {
    (void)off;
    proc_cmd_t *c = (proc_cmd_t *)node->priv;
    if (!c || !c->cmd_fn) return -1;
    c->cmd_fn(buf);
    return (int)size;
}

static vfs_ops_t cmd_ops = { .read = _cmd_read, .write = _cmd_write };

// cmd/ directory ops
static vfs_node_t *_cmd_dir_walk(vfs_node_t *dir, const char *name) {
    (void)dir;
    for (proc_cmd_t *c = cmd_list; c; c = c->next)
        if (streq(c->name, name)) return &c->node;
    return 0;
}

static vfs_dirent_t _cmd_de;

static vfs_dirent_t *_cmd_dir_readdir(vfs_node_t *dir, uint32_t index) {
    (void)dir;
    uint32_t i = 0;
    for (proc_cmd_t *c = cmd_list; c; c = c->next) {
        if (i++ == index) {
            strlcpy(_cmd_de.name, c->name, 128);
            _cmd_de.inode = i;
            return &_cmd_de;
        }
    }
    return 0;
}

static void _cmd_dir_listdir(vfs_node_t *dir) {
    (void)dir;
    for (proc_cmd_t *c = cmd_list; c; c = c->next) {
        kprint("  "); kprint(c->name); kprint("\n");
    }
}

static vfs_ops_t cmd_dir_ops = {
    .walk    = _cmd_dir_walk,
    .readdir = _cmd_dir_readdir,
    .listdir = _cmd_dir_listdir,
};

// bin/ contains mdls/.
static vfs_node_t *_bin_dir_walk(vfs_node_t *dir, const char *name) {
    (void)dir;
    if (streq(name, "mdls")) return &mdls_dir;
    return 0;
}

static vfs_dirent_t _bin_de;

static vfs_dirent_t *_bin_dir_readdir(vfs_node_t *dir, uint32_t index) {
    (void)dir;
    if (index == 0) {
        strlcpy(_bin_de.name, "mdls", 128);
        _bin_de.inode = 0;
        return &_bin_de;
    }
    return 0;
}

static void _bin_dir_listdir(vfs_node_t *dir) {
    (void)dir;
    kprint("  mdls/\n");
}

static vfs_ops_t bin_dir_ops = {
    .walk    = _bin_dir_walk,
    .readdir = _bin_dir_readdir,
    .listdir = _bin_dir_listdir,
};

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

// procfs root directory ops (cmd/ + bin/mdls/ + virtual files)
static vfs_node_t *_root_walk(vfs_node_t *dir, const char *name) {
    (void)dir;
    if (streq(name, "cmd")) return &cmd_dir;
    if (streq(name, "bin")) return &bin_dir;

    for (proc_file_t *f = file_list; f; f = f->next)
        if (streq(f->name, name)) return &f->node;
    return 0;
}

static vfs_dirent_t _root_de;

static vfs_dirent_t *_root_readdir(vfs_node_t *dir, uint32_t index) {
    (void)dir;
    if (index == 0) {
        strlcpy(_root_de.name, "cmd", 128);
        _root_de.inode = 0;
        return &_root_de;
    }
    if (index == 1) {
        strlcpy(_root_de.name, "bin", 128);
        _root_de.inode = 1;
        return &_root_de;
    }
    uint32_t i = 2;
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
    kprint("  cmd/\n");
    kprint("  bin/\n");
    for (proc_file_t *f = file_list; f; f = f->next) {
        kprint("  "); kprint(f->name); kprint("\n");
    }
}

static vfs_ops_t root_ops = {
    .walk    = _root_walk,
    .readdir = _root_readdir,
    .listdir = _root_listdir,
};

// cpuid wrapper (leaf only)
static void _cpuid(uint32_t leaf, uint32_t *eax, uint32_t *ebx,
                   uint32_t *ecx, uint32_t *edx) {
    __asm__ volatile("cpuid"
        : "=a"(*eax), "=b"(*ebx), "=c"(*ecx), "=d"(*edx)
        : "a"(leaf) : );
}

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

// /proc/cpuinfo generator
static int _cpuinfo_read(uint32_t off, uint32_t size, char *buf) {
    char tmp[512];
    int  p = 0;

    uint32_t eax, ebx, ecx, edx;
    _cpuid(0, &eax, &ebx, &ecx, &edx);
    uint32_t max_leaf = eax;
    char vendor[13];
    ((uint32_t*)vendor)[0] = ebx;
    ((uint32_t*)vendor)[1] = edx;
    ((uint32_t*)vendor)[2] = ecx;
    vendor[12] = '\0';

    char brand[49];
    memset(brand, 0, sizeof(brand));
    uint32_t max_ext;
    _cpuid(0x80000000, &max_ext, &ebx, &ecx, &edx);
    if (max_ext >= 0x80000004) {
        uint32_t *bp = (uint32_t*)brand;
        _cpuid(0x80000002, &bp[0],  &bp[1],  &bp[2],  &bp[3]);
        _cpuid(0x80000003, &bp[4],  &bp[5],  &bp[6],  &bp[7]);
        _cpuid(0x80000004, &bp[8],  &bp[9],  &bp[10], &bp[11]);
    } else {
        if (max_leaf >= 1) {
            _cpuid(1, &eax, &ebx, &ecx, &edx);
            uint32_t family = (eax >> 8) & 0xF;
            uint32_t model  = (eax >> 4) & 0xF;
            const char *s = "x86 family ";
            int i = 0;
            while (s[i]) {
                brand[i] = s[i];
                i++;
            }
            char nb[8]; itoa((int)family, nb);
            int j = 0; while (nb[j]) brand[i++] = nb[j++];
            brand[i++] = ' '; brand[i++] = 'm';
            brand[i++] = 'o'; brand[i++] = 'd';
            brand[i++] = 'e'; brand[i++] = 'l';
            brand[i++] = ' ';
            itoa((int)model, nb); j=0;
            while (nb[j]) brand[i++] = nb[j++];
        } else {
            const char *fb = "x86 compatible";
            int i = 0;
            while (fb[i]) {
                brand[i] = fb[i];
                i++;
            }
        }
    }

    uint32_t mhz = _tsc_mhz();

    uint32_t flags_edx = 0, flags_ecx = 0;
    if (max_leaf >= 1) {
        _cpuid(1, &eax, &ebx, &flags_ecx, &flags_edx);
    }

    uint32_t cache_kb = 0;
    if (max_ext >= 0x80000006) {
        uint32_t c6;
        _cpuid(0x80000006, &eax, &ebx, &c6, &edx);
        cache_kb = (c6 >> 16) & 0xFFFF;
    }

    #define _APP(s) { const char *_s=(s); while(*_s) tmp[p++]=*_s++; }
    #define _APPN(n) { char _nb[16]; itoa((int)(n),_nb); _APP(_nb); }

    _APP("processor   : 0\n");
    _APP("vendor_id   : "); _APP(vendor); _APP("\n");
    _APP("model name  : ");
    { int bi=0; while(brand[bi]==' ') bi++;
      const char *b=brand+bi; while(*b) tmp[p++]=*b++; }
    _APP("\n");
    _APP("cpu MHz     : "); _APPN(mhz); _APP("\n");
    if (cache_kb > 0) {
        _APP("cache size  : "); _APPN(cache_kb); _APP(" KB\n");
    } else {
        _APP("cache size  : unknown\n");
    }

    _APP("flags       :");
    if (flags_edx & (1<<0))  _APP(" fpu");
    if (flags_edx & (1<<1))  _APP(" vme");
    if (flags_edx & (1<<2))  _APP(" de");
    if (flags_edx & (1<<3))  _APP(" pse");
    if (flags_edx & (1<<4))  _APP(" tsc");
    if (flags_edx & (1<<5))  _APP(" msr");
    if (flags_edx & (1<<6))  _APP(" pae");
    if (flags_edx & (1<<8))  _APP(" cx8");
    if (flags_edx & (1<<9))  _APP(" apic");
    if (flags_edx & (1<<11)) _APP(" sep");
    if (flags_edx & (1<<12)) _APP(" mtrr");
    if (flags_edx & (1<<13)) _APP(" pge");
    if (flags_edx & (1<<15)) _APP(" cmov");
    if (flags_edx & (1<<19)) _APP(" clflush");
    if (flags_edx & (1<<23)) _APP(" mmx");
    if (flags_edx & (1<<25)) _APP(" sse");
    if (flags_edx & (1<<26)) _APP(" sse2");
    if (flags_edx & (1<<28)) _APP(" ht");
    if (flags_ecx & (1<<0))  _APP(" sse3");
    if (flags_ecx & (1<<9))  _APP(" ssse3");
    if (flags_ecx & (1<<19)) _APP(" sse4_1");
    if (flags_ecx & (1<<20)) _APP(" sse4_2");
    if (flags_ecx & (1<<28)) _APP(" avx");
    if (flags_ecx & (1<<30)) _APP(" rdrand");
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

// Register a read/write command node under /proc/cmd
int procfs_register_cmd(const char *name,
                         procfs_read_fn read_fn,
                         procfs_cmd_fn  cmd_fn) {
    if (!name || !cmd_fn) return -1;
    for (proc_cmd_t *c = cmd_list; c; c = c->next)
        if (streq(c->name, name)) return -1;

    proc_cmd_t *c = (proc_cmd_t *)kmalloc(sizeof(proc_cmd_t));
    if (!c) return -1;
    memset(c, 0, sizeof(proc_cmd_t));

    strlcpy(c->name, name, 64);
    c->help_fn = read_fn;
    c->cmd_fn  = cmd_fn;

    memset(&c->node, 0, sizeof(vfs_node_t));
    strlcpy(c->node.name, name, 128);
    c->node.type = VFS_FILE;
    c->node.ops  = &cmd_ops;
    c->node.priv = c;

    c->next  = cmd_list;
    cmd_list = c;
    return 0;
}

// Unregister a virtual file
int procfs_unregister_file(const char *name) {
    proc_file_t **pp = &file_list;
    while (*pp) {
        if (streq((*pp)->name, name)) {
            proc_file_t *dead = *pp;
            *pp = dead->next;
            kfree_heap(dead);
            return 0;
        }
        pp = &(*pp)->next;
    }
    return -1;
}

// Unregister a command node
int procfs_unregister_cmd(const char *name) {
    proc_cmd_t **pp = &cmd_list;
    while (*pp) {
        if (streq((*pp)->name, name)) {
            proc_cmd_t *dead = *pp;
            *pp = dead->next;
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
    memset(&cmd_dir, 0, sizeof(vfs_node_t));
    strlcpy(cmd_dir.name, "cmd", 128);
    cmd_dir.type = VFS_DIRECTORY;
    cmd_dir.ops  = &cmd_dir_ops;

    memset(&bin_dir, 0, sizeof(vfs_node_t));
    strlcpy(bin_dir.name, "bin", 128);
    bin_dir.type = VFS_DIRECTORY;
    bin_dir.ops  = &bin_dir_ops;

    memset(&mdls_dir, 0, sizeof(vfs_node_t));
    strlcpy(mdls_dir.name, "mdls", 128);
    mdls_dir.type = VFS_DIRECTORY;
    mdls_dir.ops  = &mdls_dir_ops;

    memset(&procfs_root, 0, sizeof(vfs_node_t));
    strlcpy(procfs_root.name, "proc", 128);
    procfs_root.type = VFS_DIRECTORY;
    procfs_root.ops  = &root_ops;

    procfs_register_file("cpuinfo", _cpuinfo_read);
    procfs_register_file("meminfo", _meminfo_read);
    procfs_register_file("uptime",  _uptime_read);
    procfs_register_file("version", _version_read);
    procfs_register_file("tasks",   _tasks_read);

    procfs_ready = 1;
}