#include "procfs.h"
#include "vfs.h"
#include "kernel.h"
#include "memory.h"
#include "libc.h"
#include "task.h"


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
    pos = buf_append(tmp, pos, 512, "MemFree:   ");
    pos = buf_append_int(tmp, pos, 512, (int)free_mem);
    pos = buf_append(tmp, pos, 512, " B\n");
    pos = buf_append(tmp, pos, 512, "MemTotal:  unknown\n");
    pos = buf_append(tmp, pos, 512, "SwapTotal: 0 B\n");
    pos = buf_append(tmp, pos, 512, "SwapFree:  0 B\n");
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
    pos = buf_append_int(tmp, pos, 64, (int)secs);
    pos = buf_append(tmp, pos, 64, " seconds\n");
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


#define CMD_MAX 64

typedef struct {
    char            name[64];
    char            help[128];
    void          (*handler)(char* args);
    struct vfs_node node;
} cmd_entry_t;

static cmd_entry_t   cmd_table[CMD_MAX];
static int           cmd_count = 0;
static struct vfs_node cmd_dir_node;


static int _cmd_read(struct vfs_node* node, unsigned int off,
                     unsigned int size, char* buf)
{
    cmd_entry_t* e = (cmd_entry_t*)node->ptr;
    if (!e) return 0;
    char tmp[256];
    int pos = 0;
    pos = buf_append(tmp, pos, 256, e->name);
    pos = buf_append(tmp, pos, 256, " - ");
    pos = buf_append(tmp, pos, 256, e->help);
    pos = buf_append(tmp, pos, 256, "\n");
    unsigned int len = (unsigned int)pos;
    if (off >= len) return 0;
    if (size > len - off) size = len - off;
    memcpy(buf, tmp + off, size);
    return (int)size;
}

static struct vfs_node* _cmd_finddir(struct vfs_node* node, char* name) {
    (void)node;
    for (int i = 0; i < cmd_count; i++)
        if (strncmp(cmd_table[i].name, name) == 0)
            return &cmd_table[i].node;
    return 0;
}

static void _cmd_listdir(struct vfs_node* node) {
    (void)node;
    for (int i = 0; i < cmd_count; i++) {
        kprint("  ");
        kprint(cmd_table[i].name);
        kprint("\n");
    }
}

static struct vfs_dirent _cmd_dirent;

static struct vfs_dirent* _cmd_readdir(struct vfs_node* node, unsigned int index) {
    (void)node;
    if (index >= (unsigned int)cmd_count) return 0;
    strncpy(_cmd_dirent.name, cmd_table[index].name, 128);
    _cmd_dirent.inode = (unsigned int)index;
    return &_cmd_dirent;
}


int procfs_register_command(const char* name, const char* help,
                            void (*handler)(char* args))
{
    if (!name || !handler || cmd_count >= CMD_MAX) return -1;
    for (int i = 0; i < cmd_count; i++)
        if (strncmp(cmd_table[i].name, name) == 0) return -1;

    cmd_entry_t* e = &cmd_table[cmd_count];
    strncpy(e->name, name, 64);
    strncpy(e->help, help ? help : "", 128);
    e->handler = handler;

    memset(&e->node, 0, sizeof(struct vfs_node));
    strncpy(e->node.name, name, 128);
    e->node.type = VFS_FILE;
    e->node.read = _cmd_read;
    e->node.ptr  = (struct vfs_node*)e;

    cmd_count++;
    return 0;
}

struct vfs_node* procfs_find_command(const char* name) {
    for (int i = 0; i < cmd_count; i++)
        if (strncmp(cmd_table[i].name, name) == 0)
            return &cmd_table[i].node;
    return 0;
}

void procfs_exec_command(struct vfs_node* node, char* args) {
    if (!node) return;
    cmd_entry_t* e = (cmd_entry_t*)node->ptr;
    if (e && e->handler) e->handler(args);
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
        if (strncmp(proc_files[i].name, name) == 0)
            return &proc_file_nodes[i];
    if (strncmp(name, "commands") == 0)
        return &cmd_dir_node;
    return 0;
}

static void _proc_listdir(struct vfs_node* node) {
    (void)node;
    for (int i = 0; i < (int)PROC_FILE_COUNT; i++) {
        kprint("  ");
        kprint((char*)proc_files[i].name);
        kprint("\n");
    }
    kprint("  commands/\n");
}

static struct vfs_dirent _proc_dirent;

static struct vfs_dirent* _proc_readdir(struct vfs_node* node, unsigned int index) {
    (void)node;
    if (index < PROC_FILE_COUNT) {
        strncpy(_proc_dirent.name, proc_files[index].name, 128);
        _proc_dirent.inode = index;
        return &_proc_dirent;
    }
    if (index == PROC_FILE_COUNT) {
        strncpy(_proc_dirent.name, "commands", 128);
        _proc_dirent.inode = (unsigned int)index;
        return &_proc_dirent;
    }
    return 0;
}


struct vfs_node* procfs_get_root(void) { return &procfs_root_node; }
struct vfs_node* procfs_get_commands_dir(void) { return &cmd_dir_node; }

void procfs_init(void) {
    memset(&cmd_dir_node, 0, sizeof(struct vfs_node));
    strncpy(cmd_dir_node.name, "commands", 128);
    cmd_dir_node.type    = VFS_DIRECTORY;
    cmd_dir_node.finddir = _cmd_finddir;
    cmd_dir_node.listdir = _cmd_listdir;
    cmd_dir_node.readdir = _cmd_readdir;

    memset(&procfs_root_node, 0, sizeof(struct vfs_node));
    strncpy(procfs_root_node.name, "proc", 128);
    procfs_root_node.type    = VFS_DIRECTORY;
    procfs_root_node.finddir = _proc_finddir;
    procfs_root_node.listdir = _proc_listdir;
    procfs_root_node.readdir = _proc_readdir;

    for (int i = 0; i < (int)PROC_FILE_COUNT; i++) {
        memset(&proc_file_nodes[i], 0, sizeof(struct vfs_node));
        strncpy(proc_file_nodes[i].name, proc_files[i].name, 128);
        proc_file_nodes[i].type = VFS_FILE;
        proc_file_nodes[i].read = proc_files[i].read;
    }

}