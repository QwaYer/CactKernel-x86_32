#include "procfs.h"
#include "vfs.h"
#include "kernel.h"
#include "memory.h"
#include "libc.h"
#include "task.h"


static int _strcmp(const char* a, const char* b) {
    while (*a && *a == *b) { a++; b++; }
    return *a - *b;
}

static void _strncpy(char* dst, const char* src, int n) {
    int i = 0;
    while (src[i] && i < n - 1) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}

static int _buf_append(char* buf, int pos, int max, const char* s) {
    while (*s && pos < max - 1) buf[pos++] = *s++;
    buf[pos] = '\0';
    return pos;
}

static int _buf_append_int(char* buf, int pos, int max, int n) {
    char tmp[32];
    itoa(n, tmp);
    return _buf_append(buf, pos, max, tmp);
}

static int _cpuinfo_read(struct vfs_node* node, unsigned int off,
                         unsigned int size, char* buf)
{
    (void)node; (void)off;
    static const char data[] =
        "processor   : 0\n"
        "vendor_id   : LuxKernel\n"
        "model name  : x86 (i686 compatible)\n"
        "cpu MHz     : unknown\n"
        "cache size  : unknown\n"
        "flags       : fpu de pse tsc msr pae cx8 apic\n";
    unsigned int len = sizeof(data) - 1;
    if (off >= len) return 0;
    if (size > len - off) size = len - off;
    memcpy(buf, data + off, size);
    return (int)size;
}

static int _meminfo_read(struct vfs_node* node, unsigned int off,
                         unsigned int size, char* buf)
{
    (void)node; (void)off;
    char tmp[512];
    int  pos = 0;
    unsigned int free_mem = get_free_heap_memory();

    pos = _buf_append(tmp, pos, 512, "MemFree:   ");
    pos = _buf_append_int(tmp, pos, 512, (int)free_mem);
    pos = _buf_append(tmp, pos, 512, " B\n");
    pos = _buf_append(tmp, pos, 512, "MemTotal:  unknown\n");
    pos = _buf_append(tmp, pos, 512, "SwapTotal: 0 B\n");
    pos = _buf_append(tmp, pos, 512, "SwapFree:  0 B\n");

    unsigned int len = (unsigned int)pos;
    if (off >= len) return 0;
    if (size > len - off) size = len - off;
    memcpy(buf, tmp + off, size);
    return (int)size;
}

extern unsigned int timer_ticks_get(void); 

static int _uptime_read(struct vfs_node* node, unsigned int off,
                        unsigned int size, char* buf)
{
    (void)node; (void)off;
    char tmp[64];
    int  pos = 0;
    unsigned int secs = timer_ticks_get() / 100; 
    pos = _buf_append_int(tmp, pos, 64, (int)secs);
    pos = _buf_append(tmp, pos, 64, " seconds\n");
    unsigned int len = (unsigned int)pos;
    if (off >= len) return 0;
    if (size > len - off) size = len - off;
    memcpy(buf, tmp + off, size);
    return (int)size;
}

static int _version_read(struct vfs_node* node, unsigned int off,
                         unsigned int size, char* buf)
{
    (void)node; (void)off;
    static const char data[] = "Lux Kernel 0.1.0 (x86, built with GCC)\n";
    unsigned int len = sizeof(data) - 1;
    if (off >= len) return 0;
    if (size > len - off) size = len - off;
    memcpy(buf, data + off, size);
    return (int)size;
}

static int _tasks_read(struct vfs_node* node, unsigned int off,
                       unsigned int size, char* buf)
{
    (void)node; (void)off;
    static const char data[] = "PID  NAME\n(use 'ps' command for live list)\n";
    unsigned int len = sizeof(data) - 1;
    if (off >= len) return 0;
    if (size > len - off) size = len - off;
    memcpy(buf, data + off, size);
    return (int)size;
}

typedef struct {
    const char* name;
    int (*read)(struct vfs_node*, unsigned int, unsigned int, char*);
} proc_entry_t;

static proc_entry_t proc_files[] = {
    { "cpuinfo", _cpuinfo_read },
    { "meminfo", _meminfo_read },
    { "uptime",  _uptime_read  },
    { "version", _version_read },
    { "tasks",   _tasks_read   },
};
#define PROC_FILE_COUNT (sizeof(proc_files) / sizeof(proc_entry_t))

static struct vfs_node proc_file_nodes[PROC_FILE_COUNT];

static struct vfs_node procfs_root_node;

static struct vfs_node* _proc_finddir(struct vfs_node* node, char* name) {
    (void)node;
    for (int i = 0; i < (int)PROC_FILE_COUNT; i++)
        if (_strcmp(proc_files[i].name, name) == 0)
            return &proc_file_nodes[i];
    return 0;
}

static void _proc_listdir(struct vfs_node* node) {
    (void)node;
    for (int i = 0; i < (int)PROC_FILE_COUNT; i++) {
        kprint("  ");
        kprint((char*)proc_files[i].name);
        kprint("\n");
    }
}

static struct vfs_dirent _proc_dirent;

static struct vfs_dirent* _proc_readdir(struct vfs_node* node, unsigned int index) {
    (void)node;
    if (index >= PROC_FILE_COUNT) return 0;
    _strncpy(_proc_dirent.name, proc_files[index].name, 128);
    _proc_dirent.inode = index;
    return &_proc_dirent;
}

struct vfs_node* procfs_get_root(void) {
    return &procfs_root_node;
}

void procfs_init(void) {
    memset(&procfs_root_node, 0, sizeof(struct vfs_node));
    _strncpy(procfs_root_node.name, "proc", 128);
    procfs_root_node.type    = VFS_DIRECTORY;
    procfs_root_node.finddir = _proc_finddir;
    procfs_root_node.listdir = _proc_listdir;
    procfs_root_node.readdir = _proc_readdir;

    for (int i = 0; i < (int)PROC_FILE_COUNT; i++) {
        memset(&proc_file_nodes[i], 0, sizeof(struct vfs_node));
        _strncpy(proc_file_nodes[i].name, proc_files[i].name, 128);
        proc_file_nodes[i].type = VFS_FILE;
        proc_file_nodes[i].read = proc_files[i].read;
    }

    if (vfs_root)
        vfs_mount(vfs_root, "proc", &procfs_root_node);
}