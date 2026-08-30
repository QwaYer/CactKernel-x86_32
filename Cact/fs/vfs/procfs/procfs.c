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

proc_file_t *file_list = 0;

vfs_node_t procfs_root;
static int procfs_ready = 0;

// File read: delegate to the registered read_fn
static int _file_read(vfs_node_t *node, uint32_t off, uint32_t size, char *buf) {
    proc_file_t *f = (proc_file_t *)node->priv;
    if (!f || !f->read_fn) return 0;
    return f->read_fn(off, size, buf);
}

static vfs_ops_t file_ops = { .read = _file_read };

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
    printk("  mdls/\n");
    for (proc_file_t *f = file_list; f; f = f->next) {
        printk("  "); printk(f->name); printk("\n");
    }
}

static vfs_ops_t root_ops = {
    .walk    = _root_walk,
    .readdir = _root_readdir,
    .listdir = _root_listdir,
};

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
            kfree(dead);
            return 0;
        }
        pp = &(*pp)->next;
    }
    return -1;
}

// Initialize procfs root, subdirs, and default files.
void procfs_init(void) {
    if (procfs_ready) return;
    procfs_mdls_init();

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
