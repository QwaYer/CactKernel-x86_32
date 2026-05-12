#include "sbinfs.h"
#include "vfs.h"
#include "kernel.h"
#include "memory.h"
#include "klib.h"
#include "pci_modblob.h"

/* sbinfs: ext4 /sbin + cctkfs overlay entries named /sbin/<file> (pci_modblob). */
static vfs_node_t  sbinfs_root;
static vfs_node_t *ext4_root    = 0;
static int         sbinfs_ready = 0;

typedef struct sbin_blob {
    vfs_node_t         node;
    const uint8_t     *data;
    uint32_t           size;
    int                shadowed;
    struct sbin_blob  *next;
} sbin_blob_t;

static sbin_blob_t *sbin_blobs;
static uint32_t     sbinfs_disk_sbin_count;

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

static vfs_node_t *_sbin_dir(void) {
    if (!ext4_root || !ext4_root->ops || !ext4_root->ops->walk) return 0;
    return ext4_root->ops->walk(ext4_root, "sbin");
}

static int sbin_blob_read(vfs_node_t *node, uint32_t off, uint32_t size,
                          char *buf) {
    sbin_blob_t *b = (sbin_blob_t *)node->priv;
    if (!b || !buf) return 0;
    if (off >= b->size) return 0;
    uint32_t avail = b->size - off;
    uint32_t n = size < avail ? size : avail;
    memcpy(buf, (const char *)b->data + off, n);
    return (int)n;
}

static vfs_ops_t sbin_blob_file_ops = {
    .read = sbin_blob_read,
};

static void sbinfs_count_disk_sbin(void) {
    vfs_node_t *sd = _sbin_dir();
    sbinfs_disk_sbin_count = 0;
    if (!sd || !sd->ops || !sd->ops->readdir) return;
    while (sd->ops->readdir(sd, sbinfs_disk_sbin_count))
        sbinfs_disk_sbin_count++;
}

static void sbinfs_register_init_sbin_blobs(void) {
    sbin_blob_t *head = 0;
    sbin_blob_t **tail = &head;

    int n = pci_modblob_count();
    for (int i = 0; i < n; i++) {
        const char *path;
        const uint8_t *data;
        uint32_t sz;
        if (pci_modblob_at(i, &path, &data, &sz) != 0) continue;
        if (!path_has_prefix(path, "/sbin/")) continue;
        const char *base = path + 6;
        if (!basename_only(base)) continue;

        sbin_blob_t *slot = (sbin_blob_t *)kmalloc(sizeof(sbin_blob_t));
        if (!slot) continue;
        memset(slot, 0, sizeof(sbin_blob_t));
        strlcpy(slot->node.name, base, 128);
        slot->node.type = VFS_FILE;
        slot->node.size = sz;
        slot->node.mode = 0755;
        slot->node.ops  = &sbin_blob_file_ops;
        slot->node.priv = slot;
        slot->data      = data;
        slot->size      = sz;
        slot->next      = 0;

        *tail = slot;
        tail = &slot->next;
    }

    sbin_blobs = head;

    for (sbin_blob_t *b = sbin_blobs; b; b = b->next) {
        vfs_node_t *sd = _sbin_dir();
        if (sd && sd->ops && sd->ops->walk &&
            sd->ops->walk(sd, b->node.name))
            b->shadowed = 1;
    }
}

static vfs_node_t *_root_walk(vfs_node_t *dir, const char *name) {
    (void)dir;
    vfs_node_t *sd = _sbin_dir();
    if (sd && sd->ops && sd->ops->walk) {
        vfs_node_t *disk = sd->ops->walk(sd, name);
        if (disk) return disk;
    }
    for (sbin_blob_t *b = sbin_blobs; b; b = b->next) {
        if (b->shadowed) continue;
        if (streq(b->node.name, name)) return &b->node;
    }
    return 0;
}

static vfs_dirent_t sup_de;
static vfs_dirent_t *_root_readdir(vfs_node_t *dir, uint32_t index) {
    (void)dir;
    vfs_node_t *sd = _sbin_dir();
    if (sd && sd->ops && sd->ops->readdir) {
        vfs_dirent_t *e = sd->ops->readdir(sd, index);
        if (e) return e;
    }
    uint32_t j = index - sbinfs_disk_sbin_count;
    for (sbin_blob_t *b = sbin_blobs; b; b = b->next) {
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
    vfs_node_t *sd = _sbin_dir();
    if (!sd || !sd->ops || !sd->ops->readdir) {
        kprint("  (empty)\n");
        return;
    }
    vfs_dirent_t *de;
    int any = 0;
    for (uint32_t i = 0; (de = sd->ops->readdir(sd, i)); i++) {
        if (de->name[0] == '.') continue;
        kprint("  "); kprint(de->name); kprint("\n");
        any = 1;
    }
    for (sbin_blob_t *b = sbin_blobs; b; b = b->next) {
        if (b->shadowed) continue;
        int dup = 0;
        for (uint32_t i = 0; (de = sd->ops->readdir(sd, i)); i++) {
            if (streq(de->name, b->node.name)) { dup = 1; break; }
        }
        if (dup) continue;
        kprint("  "); kprint(b->node.name); kprint("  [cctkfs]\n");
        any = 1;
    }
    if (!any) kprint("  (empty)\n");
}

static int _root_create(vfs_node_t *dir, const char *name) {
    (void)dir;
    vfs_node_t *sd = _sbin_dir();
    if (!sd || !sd->ops || !sd->ops->create) return -1;
    return sd->ops->create(sd, name);
}

static int _root_delete(vfs_node_t *dir, const char *name) {
    (void)dir;
    vfs_node_t *sd = _sbin_dir();
    if (!sd || !sd->ops || !sd->ops->delete) return -1;
    return sd->ops->delete(sd, name);
}

static int _root_mkdir(vfs_node_t *dir, const char *name) {
    (void)dir;
    vfs_node_t *sd = _sbin_dir();
    if (!sd || !sd->ops || !sd->ops->mkdir) return -1;
    return sd->ops->mkdir(sd, name);
}

static int _root_rmdir(vfs_node_t *dir, const char *name) {
    (void)dir;
    vfs_node_t *sd = _sbin_dir();
    if (!sd || !sd->ops || !sd->ops->rmdir) return -1;
    return sd->ops->rmdir(sd, name);
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

vfs_node_t *sbinfs_get_root(void) { return &sbinfs_root; }

void sbinfs_init(vfs_node_t *ext4_node) {
    if (sbinfs_ready) return;

    ext4_root = ext4_node;

    memset(&sbinfs_root, 0, sizeof(vfs_node_t));
    strlcpy(sbinfs_root.name, "sbin", 128);
    sbinfs_root.type = VFS_DIRECTORY;
    sbinfs_root.mode = 0755;
    sbinfs_root.ops  = &root_ops;

    if (ext4_root && ext4_root->ops && ext4_root->ops->walk) {
        if (!ext4_root->ops->walk(ext4_root, "sbin")) {
            if (ext4_root->ops->mkdir)
                ext4_root->ops->mkdir(ext4_root, "sbin");
        }
    }

    sbinfs_register_init_sbin_blobs();
    sbinfs_count_disk_sbin();

    sbinfs_ready = 1;
}
