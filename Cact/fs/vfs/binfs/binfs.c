#include "binfs.h"
#include "vfs.h"
#include "kernel.h"
#include "memory.h"
#include "klib.h"
#include "pci_modblob.h"

// binfs root node — forwards to ext4's /bin, with optional overlay files
// stored in the cctkfs multiboot module (paths like /bin/cactsole).
static vfs_node_t  binfs_root;
static vfs_node_t *ext4_root   = 0;
static int         binfs_ready = 0;

typedef struct bin_blob {
    vfs_node_t        node;
    const uint8_t    *data;
    uint32_t          size;
    int               shadowed; /* ext4 already has this name */
    struct bin_blob  *next;
} bin_blob_t;

static bin_blob_t *bin_blobs;
static uint32_t    binfs_disk_bin_count;

static int path_has_prefix(const char *s, const char *pre) {
    while (*pre) {
        if (*s++ != *pre++) return 0;
    }
    return 1;
}

static int basename_only(const char *base) {
    if (!base || !*base) return 0;
    for (const char *p = base; *p; p++) {
        if (*p == '/') return 0;
    }
    return 1;
}

// Helper: resolve ext4's /bin directory (lazy, on first use)
static vfs_node_t *_bin_dir(void) {
    if (!ext4_root || !ext4_root->ops || !ext4_root->ops->walk) return 0;
    return ext4_root->ops->walk(ext4_root, "bin");
}

static int bin_blob_read(vfs_node_t *node, uint32_t off, uint32_t size,
                         char *buf) {
    bin_blob_t *b = (bin_blob_t *)node->priv;
    if (!b || !buf) return 0;
    if (off >= b->size) return 0;
    uint32_t avail = b->size - off;
    uint32_t n = size < avail ? size : avail;
    memcpy(buf, (const char *)b->data + off, n);
    return (int)n;
}

static vfs_ops_t bin_blob_file_ops = {
    .read = bin_blob_read,
};

static void binfs_count_disk_bin(void) {
    vfs_node_t *bin = _bin_dir();
    binfs_disk_bin_count = 0;
    if (!bin || !bin->ops || !bin->ops->readdir) return;
    while (bin->ops->readdir(bin, binfs_disk_bin_count))
        binfs_disk_bin_count++;
}

static void binfs_register_init_bin_blobs(void) {
    bin_blob_t *head = 0;
    bin_blob_t **tail = &head;

    int n = pci_modblob_count();
    for (int i = 0; i < n; i++) {
        const char *path;
        const uint8_t *data;
        uint32_t sz;
        if (pci_modblob_at(i, &path, &data, &sz) != 0) continue;
        if (!path_has_prefix(path, "/bin/")) continue;
        const char *base = path + 5;
        if (!basename_only(base)) continue;

        bin_blob_t *slot = (bin_blob_t *)kmalloc(sizeof(bin_blob_t));
        if (!slot) continue;
        memset(slot, 0, sizeof(bin_blob_t));
        strlcpy(slot->node.name, base, 128);
        slot->node.type = VFS_FILE;
        slot->node.size = sz;
        slot->node.mode = 0755;
        slot->node.ops  = &bin_blob_file_ops;
        slot->node.priv = slot;
        slot->data      = data;
        slot->size      = sz;
        slot->next      = 0;

        *tail = slot;
        tail = &slot->next;
    }

    bin_blobs = head;

    for (bin_blob_t *b = bin_blobs; b; b = b->next) {
        vfs_node_t *bin = _bin_dir();
        if (bin && bin->ops && bin->ops->walk &&
            bin->ops->walk(bin, b->node.name))
            b->shadowed = 1;
    }
}

// Forward walk to ext4 /bin/<name>, then cctkfs /bin/<name>
static vfs_node_t *_root_walk(vfs_node_t *dir, const char *name) {
    (void)dir;
    vfs_node_t *bin = _bin_dir();
    if (bin && bin->ops && bin->ops->walk) {
        vfs_node_t *disk = bin->ops->walk(bin, name);
        if (disk) return disk;
    }
    for (bin_blob_t *b = bin_blobs; b; b = b->next) {
        if (b->shadowed) continue;
        if (streq(b->node.name, name)) return &b->node;
    }
    return 0;
}

static vfs_dirent_t sup_de;
static vfs_dirent_t *_root_readdir(vfs_node_t *dir, uint32_t index) {
    (void)dir;
    vfs_node_t *bin = _bin_dir();
    if (bin && bin->ops && bin->ops->readdir) {
        vfs_dirent_t *e = bin->ops->readdir(bin, index);
        if (e) return e;
    }
    uint32_t j = index - binfs_disk_bin_count;
    for (bin_blob_t *b = bin_blobs; b; b = b->next) {
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

static void _root_listdir(vfs_node_t *dir) {
    (void)dir;
    vfs_node_t *bin = _bin_dir();
    if (!bin || !bin->ops || !bin->ops->readdir) {
        kprint("  (empty)\n");
        return;
    }
    vfs_dirent_t *de;
    int any = 0;
    for (uint32_t i = 0; (de = bin->ops->readdir(bin, i)); i++) {
        if (de->name[0] == '.') continue;
        kprint("  "); kprint(de->name); kprint("\n");
        any = 1;
    }
    for (bin_blob_t *b = bin_blobs; b; b = b->next) {
        if (b->shadowed) continue;
        int dup = 0;
        for (uint32_t i = 0; (de = bin->ops->readdir(bin, i)); i++) {
            if (streq(de->name, b->node.name)) { dup = 1; break; }
        }
        if (dup) continue;
        kprint("  "); kprint(b->node.name); kprint("  [cctkfs]\n");
        any = 1;
    }
    if (!any) kprint("  (empty)\n");
}

// Forward create/delete/mkdir/rmdir to ext4 /bin
static int _root_create(vfs_node_t *dir, const char *name) {
    (void)dir;
    vfs_node_t *bin = _bin_dir();
    if (!bin || !bin->ops || !bin->ops->create) return -1;
    return bin->ops->create(bin, name);
}

static int _root_delete(vfs_node_t *dir, const char *name) {
    (void)dir;
    vfs_node_t *bin = _bin_dir();
    if (!bin || !bin->ops || !bin->ops->delete) return -1;
    return bin->ops->delete(bin, name);
}

static int _root_mkdir(vfs_node_t *dir, const char *name) {
    (void)dir;
    vfs_node_t *bin = _bin_dir();
    if (!bin || !bin->ops || !bin->ops->mkdir) return -1;
    return bin->ops->mkdir(bin, name);
}

static int _root_rmdir(vfs_node_t *dir, const char *name) {
    (void)dir;
    vfs_node_t *bin = _bin_dir();
    if (!bin || !bin->ops || !bin->ops->rmdir) return -1;
    return bin->ops->rmdir(bin, name);
}

static vfs_ops_t root_ops = {
    .walk    = _root_walk,
    .readdir = _root_readdir,
    .listdir = _root_listdir,
    .create  = _root_create,
    .delete  = _root_delete,
    .mkdir   = _root_mkdir,
    .rmdir   = _root_rmdir,
};

vfs_node_t *binfs_get_root(void) { return &binfs_root; }

void binfs_init(vfs_node_t *ext4_node) {
    if (binfs_ready) return;

    ext4_root = ext4_node;

    memset(&binfs_root, 0, sizeof(vfs_node_t));
    strlcpy(binfs_root.name, "bin", 128);
    binfs_root.type = VFS_DIRECTORY;
    binfs_root.mode = 0755;
    binfs_root.ops  = &root_ops;

    if (ext4_root && ext4_root->ops && ext4_root->ops->walk) {
        if (!ext4_root->ops->walk(ext4_root, "bin")) {
            if (ext4_root->ops->mkdir)
                ext4_root->ops->mkdir(ext4_root, "bin");
        }
    }

    binfs_register_init_bin_blobs();
    binfs_count_disk_bin();

    binfs_ready = 1;
    klog(LOG_OK, "binfs ready — /bin available (+ cctkfs overlay)");
}
