#include "procfs.h"
#include "procfs_internal.h"
#include "vfs.h"
#include "kernel.h"
#include "memory.h"
#include "klib.h"
#include "task.h"
#include "version.h"
#include "initfs_modblob.h"
#include "cpudev.h"
#include "apic.h"
#include "msi.h"

// mdls/ is a read-only view of bundled modules.
static vfs_node_t   mdls_files[MDLS_MAX_FILES];
static int          mdls_initialised = 0;
static vfs_dirent_t _mdls_de;

vfs_node_t mdls_dir;

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
    if (initfs_modblob_at(idx, &path, &data, &total) != 0) return 0;
    if (off >= total) return 0;
    uint32_t avail = total - off;
    if (size > avail) size = avail;
    memcpy(buf, data + off, size);
    return (int)size;
}

static vfs_ops_t mdls_file_ops = { .read = _mdls_file_read };

static int _mdls_count(void) {
    int n = initfs_modblob_count();
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
        if (initfs_modblob_at(i, &path, &data, &sz) != 0) continue;
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
        printk("  "); printk(mdls_files[i].name); printk("\n");
    }
}

vfs_ops_t mdls_dir_ops = {
    .walk    = _mdls_dir_walk,
    .readdir = _mdls_dir_readdir,
    .listdir = _mdls_dir_listdir,
};

void procfs_mdls_init(void) {
    memset(&mdls_dir, 0, sizeof(vfs_node_t));
    strlcpy(mdls_dir.name, "mdls", 128);
    mdls_dir.type = VFS_DIRECTORY;
    mdls_dir.ops  = &mdls_dir_ops;
}
