#include "usrfs.h"
#include "vfs.h"
#include "kernel.h"
#include "memory.h"
#include "klib.h"
#include "initfs_modblob.h"

static vfs_node_t  usrfs_root;
static vfs_node_t *ext4_root   = 0;
static int         usrfs_ready = 0;

typedef struct usr_blob {
    vfs_node_t        node;
    const uint8_t    *data;
    uint32_t          size;
    int               shadowed;
    struct usr_blob  *next;
} usr_blob_t;

static usr_blob_t *usr_blobs;
static uint32_t    usrfs_disk_count;

typedef struct inc_file {
    vfs_node_t       node;
    const uint8_t   *data;
    uint32_t         size;
    struct inc_file *next;
} inc_file_t;

typedef struct {
    vfs_node_t  node;
    inc_file_t *files;
} usr_inc_dir_t;

static usr_inc_dir_t usr_inc_dir;
static vfs_dirent_t inc_de;

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

static vfs_node_t *_usr_dir(void) {
    if (!ext4_root || !ext4_root->ops || !ext4_root->ops->walk) return 0;
    return ext4_root->ops->walk(ext4_root, "usr");
}

static int usr_blob_read(vfs_node_t *node, uint32_t off, uint32_t size,
                         char *buf) {
    usr_blob_t *b = (usr_blob_t *)node->priv;
    if (!b || !buf) return 0;
    if (off >= b->size) return 0;
    uint32_t avail = b->size - off;
    uint32_t n = size < avail ? size : avail;
    memcpy(buf, (const char *)b->data + off, n);
    return (int)n;
}

static vfs_ops_t usr_blob_file_ops = {
    .read = usr_blob_read,
};

static int inc_file_read(vfs_node_t *node, uint32_t off, uint32_t size,
                         char *buf) {
    inc_file_t *f = (inc_file_t *)node->priv;
    if (!f || !buf) return 0;
    if (off >= f->size) return 0;
    uint32_t avail = f->size - off;
    uint32_t n = size < avail ? size : avail;
    memcpy(buf, (const char *)f->data + off, n);
    return (int)n;
}

static vfs_ops_t inc_file_ops = {
    .read = inc_file_read,
};

static void usrfs_count_disk(void) {
    vfs_node_t *usr = _usr_dir();
    usrfs_disk_count = 0;
    if (!usr || !usr->ops || !usr->ops->readdir) return;
    while (usr->ops->readdir(usr, usrfs_disk_count))
        usrfs_disk_count++;
}

static void usrfs_register_blobs(void) {
    usr_blob_t *head = 0;
    usr_blob_t **tail = &head;
    inc_file_t *ih = 0, **it = &ih;

    int n = initfs_modblob_count();
    for (int i = 0; i < n; i++) {
        const char *path;
        const uint8_t *data;
        uint32_t sz;
        if (initfs_modblob_at(i, &path, &data, &sz) != 0) continue;
        if (!path_has_prefix(path, "/usr/")) continue;
        if (has_suffix(path, ".cctk")) continue;
        const char *base = path + 5;

        if (path_has_prefix(base, "include/")) {
            const char *inc_name = base + 8;
            if (!*inc_name) continue;
            int has_slash = 0;
            for (const char *p = inc_name; *p; p++) {
                if (*p == '/') { has_slash = 1; break; }
            }
            if (has_slash) continue;

            inc_file_t *slot = (inc_file_t *)kmalloc(sizeof(inc_file_t));
            if (!slot) continue;
            memset(slot, 0, sizeof(inc_file_t));
            strlcpy(slot->node.name, inc_name, 128);
            slot->node.type = VFS_FILE;
            slot->node.size = sz;
            slot->node.mode = 0644;
            slot->node.ops  = &inc_file_ops;
            slot->node.priv = slot;
            slot->data      = data;
            slot->size      = sz;
            *it = slot;
            it = &slot->next;
        } else {
            if (!basename_only(base)) continue;

            usr_blob_t *slot = (usr_blob_t *)kmalloc(sizeof(usr_blob_t));
            if (!slot) continue;
            memset(slot, 0, sizeof(usr_blob_t));
            strlcpy(slot->node.name, base, 128);
            slot->node.type = VFS_FILE;
            slot->node.size = sz;
            slot->node.mode = 0755;
            slot->node.ops  = &usr_blob_file_ops;
            slot->node.priv = slot;
            slot->data      = data;
            slot->size      = sz;
            slot->next      = 0;

            *tail = slot;
            tail = &slot->next;
        }
    }

    usr_blobs = head;
    usr_inc_dir.files = ih;
    memset(&usr_inc_dir.node, 0, sizeof(usr_inc_dir.node));
    strlcpy(usr_inc_dir.node.name, "include", 128);
    usr_inc_dir.node.type = VFS_DIRECTORY;
    usr_inc_dir.node.mode = 0755;
    usr_inc_dir.node.priv = &usr_inc_dir;

    for (usr_blob_t *b = usr_blobs; b; b = b->next) {
        vfs_node_t *usr = _usr_dir();
        if (usr && usr->ops && usr->ops->walk &&
            usr->ops->walk(usr, b->node.name))
            b->shadowed = 1;
    }
}

static vfs_node_t *_inc_walk(vfs_node_t *dir, const char *name) {
    usr_inc_dir_t *inc = (usr_inc_dir_t *)dir->priv;
    if (!inc) return 0;
    for (inc_file_t *f = inc->files; f; f = f->next) {
        if (streq(f->node.name, name)) return &f->node;
    }
    return 0;
}

static vfs_dirent_t *_inc_readdir(vfs_node_t *dir, uint32_t index) {
    usr_inc_dir_t *inc = (usr_inc_dir_t *)dir->priv;
    if (!inc) return 0;
    uint32_t i = 0;
    for (inc_file_t *f = inc->files; f; f = f->next) {
        if (i++ == index) {
            strlcpy(inc_de.name, f->node.name, 128);
            inc_de.inode = i;
            return &inc_de;
        }
    }
    return 0;
}

static void _inc_listdir(vfs_node_t *dir) {
    usr_inc_dir_t *inc = (usr_inc_dir_t *)dir->priv;
    if (!inc || !inc->files) { printk("  (empty)\n"); return; }
    for (inc_file_t *f = inc->files; f; f = f->next) {
        printk("  "); printk(f->node.name); printk("\n");
    }
}

static vfs_ops_t inc_dir_ops = {
    .walk    = _inc_walk,
    .readdir = _inc_readdir,
    .listdir = _inc_listdir,
};

static vfs_node_t *_root_walk(vfs_node_t *dir, const char *name) {
    (void)dir;
    if (streq(name, "include")) {
        usr_inc_dir.node.ops = &inc_dir_ops;
        return &usr_inc_dir.node;
    }
    vfs_node_t *usr = _usr_dir();
    if (usr && usr->ops && usr->ops->walk) {
        vfs_node_t *disk = usr->ops->walk(usr, name);
        if (disk) return disk;
    }
    for (usr_blob_t *b = usr_blobs; b; b = b->next) {
        if (b->shadowed) continue;
        if (streq(b->node.name, name)) return &b->node;
    }
    return 0;
}

static vfs_dirent_t sup_de;
static vfs_dirent_t *_root_readdir(vfs_node_t *dir, uint32_t index) {
    (void)dir;
    int has_inc = (usr_inc_dir.files != 0);
    vfs_node_t *usr = _usr_dir();

    if (usr && usr->ops && usr->ops->readdir) {
        vfs_dirent_t *e = usr->ops->readdir(usr, index);
        if (e) return e;
    }

    uint32_t j = index - usrfs_disk_count;
    if (has_inc) {
        if (j == 0) {
            strlcpy(sup_de.name, "include", 128);
            sup_de.inode = 0;
            return &sup_de;
        }
        j--;
    }
    for (usr_blob_t *b = usr_blobs; b; b = b->next) {
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
    vfs_node_t *usr = _usr_dir();
    vfs_dirent_t *de;
    int any = 0;
    if (usr && usr->ops && usr->ops->readdir) {
        for (uint32_t i = 0; (de = usr->ops->readdir(usr, i)); i++) {
            if (de->name[0] == '.') continue;
            printk("  "); printk(de->name); printk("\n");
            any = 1;
        }
    }
    if (usr_inc_dir.files) {
        printk("  include/  [headers]\n");
        any = 1;
    }
    for (usr_blob_t *b = usr_blobs; b; b = b->next) {
        if (b->shadowed) continue;
        int dup = 0;
        if (usr && usr->ops && usr->ops->readdir) {
            for (uint32_t i = 0; (de = usr->ops->readdir(usr, i)); i++) {
                if (streq(de->name, b->node.name)) { dup = 1; break; }
            }
        }
        if (dup) continue;
        printk("  "); printk(b->node.name); printk("  [cctkfs]\n");
        any = 1;
    }
    if (!any) printk("  (empty)\n");
}

static int _root_create(vfs_node_t *dir, const char *name) {
    (void)dir;
    vfs_node_t *usr = _usr_dir();
    if (!usr || !usr->ops || !usr->ops->create) return -1;
    return usr->ops->create(usr, name);
}

static int _root_delete(vfs_node_t *dir, const char *name) {
    (void)dir;
    vfs_node_t *usr = _usr_dir();
    if (!usr || !usr->ops || !usr->ops->delete) return -1;
    return usr->ops->delete(usr, name);
}

static int _root_mkdir(vfs_node_t *dir, const char *name) {
    (void)dir;
    vfs_node_t *usr = _usr_dir();
    if (!usr || !usr->ops || !usr->ops->mkdir) return -1;
    return usr->ops->mkdir(usr, name);
}

static int _root_rmdir(vfs_node_t *dir, const char *name) {
    (void)dir;
    vfs_node_t *usr = _usr_dir();
    if (!usr || !usr->ops || !usr->ops->rmdir) return -1;
    return usr->ops->rmdir(usr, name);
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

vfs_node_t *usrfs_get_root(void) { return &usrfs_root; }

void usrfs_init(vfs_node_t *ext4_node) {
    if (usrfs_ready) return;

    ext4_root = ext4_node;

    memset(&usrfs_root, 0, sizeof(vfs_node_t));
    strlcpy(usrfs_root.name, "usr", 128);
    usrfs_root.type = VFS_DIRECTORY;
    usrfs_root.mode = 0755;
    usrfs_root.ops  = &root_ops;

    if (ext4_root && ext4_root->ops && ext4_root->ops->walk) {
        if (!ext4_root->ops->walk(ext4_root, "usr")) {
            if (ext4_root->ops->mkdir)
                ext4_root->ops->mkdir(ext4_root, "usr");
        }
    }

    usrfs_register_blobs();
    usrfs_count_disk();

    usrfs_ready = 1;
}
