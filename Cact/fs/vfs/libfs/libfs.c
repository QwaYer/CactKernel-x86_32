#include "libfs.h"
#include "vfs.h"
#include "kernel.h"
#include "memory.h"
#include "klib.h"
#include "pci_modblob.h"

// libfs serves /lib by overlaying cctkfs blobs (e.g. libc.so) on top of
// the on-disk ext4 /lib directory. cctkfs entries with prefix "/lib/" and
// a non-.cctk suffix are registered here; .cctk PCI modules stay private
// to pci_modblob_get() and are not exposed through VFS.
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

static void libfs_count_disk(void) {
    vfs_node_t *lib = _lib_dir();
    libfs_disk_count = 0;
    if (!lib || !lib->ops || !lib->ops->readdir) return;
    while (lib->ops->readdir(lib, libfs_disk_count))
        libfs_disk_count++;
}

static void libfs_register_blobs(void) {
    lib_blob_t *head = 0;
    lib_blob_t **tail = &head;

    int n = pci_modblob_count();
    for (int i = 0; i < n; i++) {
        const char *path;
        const uint8_t *data;
        uint32_t sz;
        if (pci_modblob_at(i, &path, &data, &sz) != 0) continue;
        if (!path_has_prefix(path, "/lib/")) continue;
        /* .cctk PCI modules are dispatched through pci_modblob, not VFS. */
        if (has_suffix(path, ".cctk")) continue;
        const char *base = path + 5;
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

    lib_blobs = head;

    for (lib_blob_t *b = lib_blobs; b; b = b->next) {
        vfs_node_t *lib = _lib_dir();
        if (lib && lib->ops && lib->ops->walk &&
            lib->ops->walk(lib, b->node.name))
            b->shadowed = 1;
    }
}

// Resolve entry from ext4 first, then overlay.
static vfs_node_t *_root_walk(vfs_node_t *dir, const char *name) {
    (void)dir;
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
    if (lib && lib->ops && lib->ops->readdir) {
        for (uint32_t i = 0; (de = lib->ops->readdir(lib, i)); i++) {
            if (de->name[0] == '.') continue;
            kprint("  "); kprint(de->name); kprint("\n");
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
        kprint("  "); kprint(b->node.name); kprint("  [cctkfs]\n");
        any = 1;
    }
    if (!any) kprint("  (empty)\n");
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
