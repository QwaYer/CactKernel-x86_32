#include "libfs.h"
#include "vfs.h"
#include "kernel.h"
#include "memory.h"
#include "klib.h"
#include "initfs_modblob.h"

// libfs serves /lib by overlaying cctkfs blobs (e.g. libc.so) on top of
// the on-disk ext4 /lib directory. cctkfs entries with prefix "/lib/" and
// a non-.cctk suffix are registered here; .cctk PCI modules stay private
// to initfs_modblob_get() and are not exposed through VFS.
static vfs_node_t  libfs_root;
static vfs_node_t *ext4_root   = 0;
static int         libfs_ready = 0;

typedef struct lib_blob {
    vfs_node_t        node;
    const uint8_t    *data;
    uint32_t          size;
    int               shadowed; /* ext4 already has this name */
    struct lib_blob  *next;
} lib_blob_t;

static lib_blob_t *lib_blobs;
static uint32_t    libfs_disk_count;

// Subdirectory file entry (for include/, tcc/, sys/)
typedef struct sub_file {
    vfs_node_t        node;
    const uint8_t    *data;
    uint32_t          size;
    struct sub_file  *next;
} sub_file_t;

// Subdirectory descriptor
typedef struct {
    vfs_node_t   node;
    const char  *prefix;
    int          prefix_len;
    sub_file_t  *files;
} lib_subdir_t;

static lib_subdir_t lib_inc_dir;
static lib_subdir_t lib_tcc_dir;
static lib_subdir_t lib_sys_dir;

static int path_has_prefix(const char *s, const char *pre) {
    while (*pre) {
        if (*s++ != *pre++) return 0;
    }
    return 1;
}

static int has_suffix(const char *s, const char *suf) {
    int sl = strlen((char *)s), fl = strlen((char *)suf);
    if (fl > sl) return 0;
    return strcmp((char *)(s + sl - fl), (char *)suf) == 0;
}

static int basename_only(const char *base) {
    if (!base || !*base) return 0;
    for (const char *p = base; *p; p++) {
        if (*p == '/') return 0;
    }
    return 1;
}

// Resolve ext4 /lib lazily.
static vfs_node_t *_lib_dir(void) {
    if (!ext4_root || !ext4_root->ops || !ext4_root->ops->walk) return 0;
    return ext4_root->ops->walk(ext4_root, "lib");
}

static int lib_blob_read(vfs_node_t *node, uint32_t off, uint32_t size,
                         char *buf) {
    lib_blob_t *b = (lib_blob_t *)node->priv;
    if (!b || !buf) return 0;
    if (off >= b->size) return 0;
    uint32_t avail = b->size - off;
    uint32_t n = size < avail ? size : avail;
    memcpy(buf, (const char *)b->data + off, n);
    return (int)n;
}

static vfs_ops_t lib_blob_file_ops = {
    .read = lib_blob_read,
};

static int sub_file_read(vfs_node_t *node, uint32_t off, uint32_t size,
                         char *buf) {
    sub_file_t *f = (sub_file_t *)node->priv;
    if (!f || !buf) return 0;
    if (off >= f->size) return 0;
    uint32_t avail = f->size - off;
    uint32_t n = size < avail ? size : avail;
    memcpy(buf, (const char *)f->data + off, n);
    return (int)n;
}

static vfs_ops_t sub_file_ops = {
    .read = sub_file_read,
};

static void libfs_count_disk(void) {
    vfs_node_t *lib = _lib_dir();
    libfs_disk_count = 0;
    if (!lib || !lib->ops || !lib->ops->readdir) return;
    while (lib->ops->readdir(lib, libfs_disk_count))
        libfs_disk_count++;
}

// Register cctkfs blobs under /lib/
// Flat files (libc.so, hello.c, ...) → lib_blobs
// Subdirectory files (include/*, tcc/*, sys/*) → subdir file lists
static void libfs_register_blobs(void) {
    lib_blob_t *head = 0;
    lib_blob_t **tail = &head;

    sub_file_t *ih = 0, **it = &ih;
    sub_file_t *th = 0, **tt = &th;
    sub_file_t *sh = 0, **st = &sh;

    int n = initfs_modblob_count();
    for (int i = 0; i < n; i++) {
        const char *path;
        const uint8_t *data;
        uint32_t sz;
        if (initfs_modblob_at(i, &path, &data, &sz) != 0) continue;
        if (!path_has_prefix(path, "/lib/")) continue;
        if (has_suffix(path, ".cctk")) continue;
        const char *base = path + 5;

        // Check for known subdirectories
        if (path_has_prefix(base, "include/")) {
            const char *name = base + 8;
            if (!*name) continue;
            if (!basename_only(name)) continue;
            sub_file_t *slot = (sub_file_t *)kmalloc(sizeof(sub_file_t));
            if (!slot) continue;
            memset(slot, 0, sizeof(sub_file_t));
            strlcpy(slot->node.name, name, 128);
            slot->node.type = VFS_FILE;
            slot->node.size = sz;
            slot->node.mode = 0644;
            slot->node.ops  = &sub_file_ops;
            slot->node.priv = slot;
            slot->data      = data;
            slot->size      = sz;
            *it = slot;
            it = &slot->next;
        } else if (path_has_prefix(base, "tcc/")) {
            const char *name = base + 4;
            if (!*name) continue;
            if (!basename_only(name)) continue;
            sub_file_t *slot = (sub_file_t *)kmalloc(sizeof(sub_file_t));
            if (!slot) continue;
            memset(slot, 0, sizeof(sub_file_t));
            strlcpy(slot->node.name, name, 128);
            slot->node.type = VFS_FILE;
            slot->node.size = sz;
            slot->node.mode = 0644;
            slot->node.ops  = &sub_file_ops;
            slot->node.priv = slot;
            slot->data      = data;
            slot->size      = sz;
            *tt = slot;
            tt = &slot->next;
        } else if (path_has_prefix(base, "sys/")) {
            const char *name = base + 4;
            if (!*name) continue;
            if (!basename_only(name)) continue;
            sub_file_t *slot = (sub_file_t *)kmalloc(sizeof(sub_file_t));
            if (!slot) continue;
            memset(slot, 0, sizeof(sub_file_t));
            strlcpy(slot->node.name, name, 128);
            slot->node.type = VFS_FILE;
            slot->node.size = sz;
            slot->node.mode = 0644;
            slot->node.ops  = &sub_file_ops;
            slot->node.priv = slot;
            slot->data      = data;
            slot->size      = sz;
            *st = slot;
            st = &slot->next;
        } else {
            if (!basename_only(base)) continue;
            lib_blob_t *slot = (lib_blob_t *)kmalloc(sizeof(lib_blob_t));
            if (!slot) continue;
            memset(slot, 0, sizeof(lib_blob_t));
            strlcpy(slot->node.name, base, 128);
            slot->node.type = VFS_FILE;
            slot->node.size = sz;
            slot->node.mode = 0755;
            slot->node.ops  = &lib_blob_file_ops;
            slot->node.priv = slot;
            slot->data      = data;
            slot->size      = sz;
            slot->next      = 0;
            *tail = slot;
            tail = &slot->next;
        }
    }

    lib_blobs = head;

    // Init include subdirectory node
    lib_inc_dir.files = ih;
    memset(&lib_inc_dir.node, 0, sizeof(lib_inc_dir.node));
    strlcpy(lib_inc_dir.node.name, "include", 128);
    lib_inc_dir.node.type = VFS_DIRECTORY;
    lib_inc_dir.node.mode = 0755;
    lib_inc_dir.node.priv = &lib_inc_dir;
    lib_inc_dir.prefix    = "include/";
    lib_inc_dir.prefix_len = 8;

    // Init tcc subdirectory node
    lib_tcc_dir.files = th;
    memset(&lib_tcc_dir.node, 0, sizeof(lib_tcc_dir.node));
    strlcpy(lib_tcc_dir.node.name, "tcc", 128);
    lib_tcc_dir.node.type = VFS_DIRECTORY;
    lib_tcc_dir.node.mode = 0755;
    lib_tcc_dir.node.priv = &lib_tcc_dir;
    lib_tcc_dir.prefix    = "tcc/";
    lib_tcc_dir.prefix_len = 4;

    // Init sys subdirectory node
    lib_sys_dir.files = sh;
    memset(&lib_sys_dir.node, 0, sizeof(lib_sys_dir.node));
    strlcpy(lib_sys_dir.node.name, "sys", 128);
    lib_sys_dir.node.type = VFS_DIRECTORY;
    lib_sys_dir.node.mode = 0755;
    lib_sys_dir.node.priv = &lib_sys_dir;
    lib_sys_dir.prefix    = "sys/";
    lib_sys_dir.prefix_len = 4;

    for (lib_blob_t *b = lib_blobs; b; b = b->next) {
        vfs_node_t *lib = _lib_dir();
        if (lib && lib->ops && lib->ops->walk &&
            lib->ops->walk(lib, b->node.name))
            b->shadowed = 1;
    }
}

// Subdirectory walk handler
static vfs_node_t *_sub_walk(vfs_node_t *dir, const char *name) {
    lib_subdir_t *sd = (lib_subdir_t *)dir->priv;
    if (!sd) return 0;
    for (sub_file_t *f = sd->files; f; f = f->next) {
        if (streq(f->node.name, name)) return &f->node;
    }
    return 0;
}

static vfs_dirent_t sub_de;
static vfs_dirent_t *_sub_readdir(vfs_node_t *dir, uint32_t index) {
    lib_subdir_t *sd = (lib_subdir_t *)dir->priv;
    if (!sd) return 0;
    uint32_t i = 0;
    for (sub_file_t *f = sd->files; f; f = f->next) {
        if (i++ == index) {
            strlcpy(sub_de.name, f->node.name, 128);
            sub_de.inode = i;
            return &sub_de;
        }
    }
    return 0;
}

static void _sub_listdir(vfs_node_t *dir) {
    lib_subdir_t *sd = (lib_subdir_t *)dir->priv;
    if (!sd || !sd->files) { printk("  (empty)\n"); return; }
    for (sub_file_t *f = sd->files; f; f = f->next) {
        printk("  "); printk(f->node.name); printk("\n");
    }
}

static vfs_ops_t sub_dir_ops = {
    .walk    = _sub_walk,
    .readdir = _sub_readdir,
    .listdir = _sub_listdir,
};

// Resolve entry from ext4 first, then overlay.
static vfs_node_t *_root_walk(vfs_node_t *dir, const char *name) {
    (void)dir;
    if (streq(name, "include")) {
        lib_inc_dir.node.ops = &sub_dir_ops;
        return &lib_inc_dir.node;
    }
    if (streq(name, "tcc")) {
        lib_tcc_dir.node.ops = &sub_dir_ops;
        return &lib_tcc_dir.node;
    }
    if (streq(name, "sys")) {
        lib_sys_dir.node.ops = &sub_dir_ops;
        return &lib_sys_dir.node;
    }
    vfs_node_t *lib = _lib_dir();
    if (lib && lib->ops && lib->ops->walk) {
        vfs_node_t *disk = lib->ops->walk(lib, name);
        if (disk) return disk;
    }
    for (lib_blob_t *b = lib_blobs; b; b = b->next) {
        if (b->shadowed) continue;
        if (streq(b->node.name, name)) return &b->node;
    }
    return 0;
}

static vfs_dirent_t sup_de;
static vfs_dirent_t *_root_readdir(vfs_node_t *dir, uint32_t index) {
    (void)dir;
    vfs_node_t *lib = _lib_dir();
    if (lib && lib->ops && lib->ops->readdir) {
        vfs_dirent_t *e = lib->ops->readdir(lib, index);
        if (e) return e;
    }
    uint32_t j = index - libfs_disk_count;
    // Emit subdirectories first
    for (int s = 0; s < 3; s++) {
        const char *dname = s == 0 ? "include" : (s == 1 ? "tcc" : "sys");
        if (j == 0) {
            strlcpy(sup_de.name, dname, 128);
            sup_de.inode = 0;
            return &sup_de;
        }
        j--;
    }
    for (lib_blob_t *b = lib_blobs; b; b = b->next) {
        if (b->shadowed) continue;
        if (j == 0) {
            strlcpy(sup_de.name, b->node.name, 128);
            sup_de.inode = 0;
            return &sup_de;
        }
        j--;
    }
    return 0;
}

// List ext4 /lib + cctkfs overlay with empty fallback.
static void _root_listdir(vfs_node_t *dir) {
    (void)dir;
    vfs_node_t *lib = _lib_dir();
    vfs_dirent_t *de;
    int any = 0;

    // List subdirectories
    if (lib_inc_dir.files) { printk("  include\n"); any = 1; }
    if (lib_tcc_dir.files) { printk("  tcc\n"); any = 1; }
    if (lib_sys_dir.files) { printk("  sys\n"); any = 1; }

    if (lib && lib->ops && lib->ops->readdir) {
        for (uint32_t i = 0; (de = lib->ops->readdir(lib, i)); i++) {
            if (de->name[0] == '.') continue;
            printk("  "); printk(de->name); printk("\n");
            any = 1;
        }
    }
    for (lib_blob_t *b = lib_blobs; b; b = b->next) {
        if (b->shadowed) continue;
        int dup = 0;
        if (lib && lib->ops && lib->ops->readdir) {
            for (uint32_t i = 0; (de = lib->ops->readdir(lib, i)); i++) {
                if (streq(de->name, b->node.name)) { dup = 1; break; }
            }
        }
        if (dup) continue;
        printk("  "); printk(b->node.name); printk("  [cctkfs]\n");
        any = 1;
    }
    if (!any) printk("  (empty)\n");
}

// Forward create/delete/mkdir/rmdir to ext4 /lib
static int _root_create(vfs_node_t *dir, const char *name) {
    (void)dir;
    vfs_node_t *lib = _lib_dir();
    if (!lib || !lib->ops || !lib->ops->create) return -1;
    return lib->ops->create(lib, name);
}

static int _root_delete(vfs_node_t *dir, const char *name) {
    (void)dir;
    vfs_node_t *lib = _lib_dir();
    if (!lib || !lib->ops || !lib->ops->delete) return -1;
    return lib->ops->delete(lib, name);
}

static int _root_mkdir(vfs_node_t *dir, const char *name) {
    (void)dir;
    vfs_node_t *lib = _lib_dir();
    if (!lib || !lib->ops || !lib->ops->mkdir) return -1;
    return lib->ops->mkdir(lib, name);
}

static int _root_rmdir(vfs_node_t *dir, const char *name) {
    (void)dir;
    vfs_node_t *lib = _lib_dir();
    if (!lib || !lib->ops || !lib->ops->rmdir) return -1;
    return lib->ops->rmdir(lib, name);
}

// VFS ops table for libfs root
static vfs_ops_t root_ops = {
    .walk    = _root_walk,
    .readdir = _root_readdir,
    .listdir = _root_listdir,
    .create  = _root_create,
    .delete  = _root_delete,
    .mkdir   = _root_mkdir,
    .rmdir   = _root_rmdir,
};

// Return the libfs root node (registered in VFS mount table)
vfs_node_t *libfs_get_root(void) { return &libfs_root; }

// Initialize libfs and ensure /lib exists.
void libfs_init(vfs_node_t *ext4_node) {
    if (libfs_ready) return;

    ext4_root = ext4_node;

    memset(&libfs_root, 0, sizeof(vfs_node_t));
    strlcpy(libfs_root.name, "lib", 128);
    libfs_root.type = VFS_DIRECTORY;
    libfs_root.mode = 0755;
    libfs_root.ops  = &root_ops;

    // Ensure /lib exists on ext4
    if (ext4_root && ext4_root->ops && ext4_root->ops->walk) {
        if (!ext4_root->ops->walk(ext4_root, "lib")) {
            if (ext4_root->ops->mkdir)
                ext4_root->ops->mkdir(ext4_root, "lib");
        }
    }

    libfs_register_blobs();
    libfs_count_disk();

    libfs_ready = 1;
}
