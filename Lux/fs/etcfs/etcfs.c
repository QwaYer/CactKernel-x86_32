#include "etcfs.h"
#include "vfs.h"
#include "kernel.h"
#include "memory.h"
#include "libc.h"


typedef struct etc_entry etc_entry_t;

struct etc_entry {
    char          name[ETCFS_NAME_LEN];
    char         *data;
    uint32_t      size;
    uint32_t      cap;      
    int           dirty;
    vfs_node_t    node;
    etc_entry_t  *next;
};

static etc_entry_t *etc_list   = 0;
static vfs_node_t   etc_root;
static vfs_node_t  *ext4_root  = 0;  
static int          etcfs_ready = 0;


static etc_entry_t *_find(const char *name) {
    for (etc_entry_t *e = etc_list; e; e = e->next)
        if (streq(e->name, name)) return e;
    return 0;
}


static vfs_node_t *_ext4_etc_dir(void) {
    if (!ext4_root || !ext4_root->ops || !ext4_root->ops->walk) return 0;
    return ext4_root->ops->walk(ext4_root, "etc");
}

static int _disk_read(const char *name, char *buf, uint32_t maxsize) {
    vfs_node_t *etc_dir = _ext4_etc_dir();
    if (!etc_dir) return 0;

    if (!etc_dir->ops || !etc_dir->ops->walk) return 0;
    vfs_node_t *file = etc_dir->ops->walk(etc_dir, name);
    if (!file) return 0;

    uint32_t sz = file->size ? file->size : maxsize;
    if (sz > maxsize) sz = maxsize;
    if (!file->ops || !file->ops->read) return 0;
    return file->ops->read(file, 0, sz, buf);
}

static int _disk_write(const char *name, const char *buf, uint32_t size) {
    if (!ext4_root) return -1;

    vfs_node_t *etc_dir = _ext4_etc_dir();
    if (!etc_dir) {
        if (!ext4_root->ops || !ext4_root->ops->mkdir) return -1;
        if (ext4_root->ops->mkdir(ext4_root, "etc") < 0) return -1;
        etc_dir = _ext4_etc_dir();
        if (!etc_dir) return -1;
    }

    if (etc_dir->ops && etc_dir->ops->walk) {
        vfs_node_t *old = etc_dir->ops->walk(etc_dir, name);
        if (old && etc_dir->ops->delete)
            etc_dir->ops->delete(etc_dir, name);
    }

    if (!etc_dir->ops || !etc_dir->ops->create) return -1;
    if (etc_dir->ops->create(etc_dir, (char*)name) < 0) return -1;

    if (size == 0) return 0;

    vfs_node_t *file = etc_dir->ops->walk(etc_dir, name);
    if (!file || !file->ops || !file->ops->write) return -1;
    return file->ops->write(file, 0, size, (char*)buf);
}


static int _ensure_cap(etc_entry_t *e, uint32_t needed) {
    if (needed <= e->cap) return 0;
    uint32_t newcap = e->cap ? e->cap * 2 : ETCFS_INIT_SIZE;
    while (newcap < needed) newcap *= 2;
    char *nb = (char *)kmalloc(newcap);
    if (!nb) return -1;
    memset(nb, 0, newcap);
    if (e->data) {
        memcpy(nb, e->data, e->size);
        kfree_heap(e->data);
    }
    e->data = nb;
    e->cap  = newcap;
    return 0;
}


static int _file_read(vfs_node_t *node, uint32_t off, uint32_t size, char *buf) {
    etc_entry_t *e = (etc_entry_t *)node->priv;
    if (!e || !e->data) return 0;
    if (off >= e->size) return 0;
    uint32_t avail = e->size - off;
    if (size > avail) size = avail;
    memcpy(buf, e->data + off, size);
    return (int)size;
}

static int _file_write(vfs_node_t *node, uint32_t off, uint32_t size, char *buf) {
    etc_entry_t *e = (etc_entry_t *)node->priv;
    if (!e) return -1;

    uint32_t needed = off + size;
    if (_ensure_cap(e, needed) < 0) return -1;

    memcpy(e->data + off, buf, size);
    if (needed > e->size) e->size = needed;
    node->size = e->size;
    e->dirty   = 1;

    _disk_write(e->name, e->data, e->size);
    e->dirty = 0;
    return (int)size;
}

static vfs_ops_t file_ops = { .read = _file_read, .write = _file_write };


static vfs_node_t *_root_walk(vfs_node_t *dir, const char *name) {
    (void)dir;
    etc_entry_t *e = _find(name);
    return e ? &e->node : 0;
}

static vfs_dirent_t _root_de;

static vfs_dirent_t *_root_readdir(vfs_node_t *dir, uint32_t index) {
    (void)dir;
    uint32_t i = 0;
    for (etc_entry_t *e = etc_list; e; e = e->next) {
        if (i++ == index) {
            strlcpy(_root_de.name, e->name, 128);
            _root_de.inode = i;
            return &_root_de;
        }
    }
    return 0;
}

static void _root_listdir(vfs_node_t *dir) {
    (void)dir;
    int any = 0;
    for (etc_entry_t *e = etc_list; e; e = e->next) {
        kprint("  "); kprint(e->name); kprint("\n");
        any = 1;
    }
    if (!any) kprint("  (empty)\n");
}

static int _root_create(vfs_node_t *dir, const char *name) {
    (void)dir;
    return etcfs_create(name);
}

static int _root_delete(vfs_node_t *dir, const char *name) {
    (void)dir;
    return etcfs_delete(name);
}

static vfs_ops_t root_ops = {
    .walk    = _root_walk,
    .readdir = _root_readdir,
    .listdir = _root_listdir,
    .create  = _root_create,
    .delete  = _root_delete,
};


static void _load_all(void) {
    vfs_node_t *etc_dir = _ext4_etc_dir();
    if (!etc_dir) {
        if (!ext4_root || !ext4_root->ops || !ext4_root->ops->mkdir) return;
        ext4_root->ops->mkdir(ext4_root, "etc");
        etc_dir = _ext4_etc_dir();
        if (!etc_dir) { kprint("[etcfs] mkdir /etc failed\n"); return; }
    }

    if (!etc_dir->ops || !etc_dir->ops->readdir) return;

    uint32_t idx = 0;
    vfs_dirent_t *de;
    while ((de = etc_dir->ops->readdir(etc_dir, idx++)) != 0) {
        if (de->name[0] == '.') continue;
        if (_find(de->name)) continue;   

        etc_entry_t *slot = (etc_entry_t *)kmalloc(sizeof(etc_entry_t));
        if (!slot) { kprint("[etcfs] kmalloc failed\n"); break; }
        memset(slot, 0, sizeof(etc_entry_t));

        slot->data = (char *)kmalloc(ETCFS_INIT_SIZE);
        if (!slot->data) { kfree_heap(slot); continue; }
        memset(slot->data, 0, ETCFS_INIT_SIZE);
        slot->cap = ETCFS_INIT_SIZE;

        int bytes = _disk_read(de->name, slot->data, ETCFS_INIT_SIZE);

        strlcpy(slot->name, de->name, ETCFS_NAME_LEN);
        slot->size  = (uint32_t)(bytes > 0 ? bytes : 0);
        slot->dirty = 0;

        memset(&slot->node, 0, sizeof(vfs_node_t));
        strlcpy(slot->node.name, de->name, 128);
        slot->node.type = VFS_FILE;
        slot->node.size = slot->size;
        slot->node.ops  = &file_ops;
        slot->node.priv = slot;

        slot->next = etc_list;
        etc_list   = slot;

        kprint("[etcfs] loaded: /etc/"); kprint(de->name); kprint("\n");
    }
}


//Public api
vfs_node_t *etcfs_get_root(void) { return &etc_root; }

void etcfs_init(vfs_node_t *ext4_node) {
    if (etcfs_ready) return;

    ext4_root = ext4_node;

    memset(&etc_root, 0, sizeof(vfs_node_t));
    strlcpy(etc_root.name, "etc", 128);
    etc_root.type = VFS_DIRECTORY;
    etc_root.ops  = &root_ops;

    etcfs_ready = 1;
    _load_all();

    if (!_find("mounts"))
        etcfs_create("mounts");
}

int etcfs_read(const char *name, char *buf, uint32_t size) {
    etc_entry_t *e = _find(name);
    if (!e || !e->data) return 0;
    uint32_t sz = e->size < size ? e->size : size;
    memcpy(buf, e->data, sz);
    return (int)sz;
}

int etcfs_write(const char *name, const char *buf, uint32_t size) {
    etc_entry_t *e = _find(name);
    if (!e) {
        if (etcfs_create(name) < 0) return -1;
        e = _find(name);
        if (!e) return -1;
    }
    if (_ensure_cap(e, size) < 0) return -1;
    memcpy(e->data, buf, size);
    e->size      = size;
    e->dirty     = 1;
    e->node.size = size;
    _disk_write(name, e->data, e->size);
    e->dirty = 0;
    return (int)size;
}

int etcfs_create(const char *name) {
    if (_find(name)) return 0;   

    etc_entry_t *e = (etc_entry_t *)kmalloc(sizeof(etc_entry_t));
    if (!e) return -1;
    memset(e, 0, sizeof(etc_entry_t));

    e->data = (char *)kmalloc(ETCFS_INIT_SIZE);
    if (!e->data) { kfree_heap(e); return -1; }
    memset(e->data, 0, ETCFS_INIT_SIZE);
    e->cap = ETCFS_INIT_SIZE;

    strlcpy(e->name, name, ETCFS_NAME_LEN);
    e->size  = 0;
    e->dirty = 0;

    memset(&e->node, 0, sizeof(vfs_node_t));
    strlcpy(e->node.name, name, 128);
    e->node.type = VFS_FILE;
    e->node.ops  = &file_ops;
    e->node.priv = e;

    e->next  = etc_list;
    etc_list = e;

    _disk_write(name, "", 0);
    return 0;
}

int etcfs_delete(const char *name) {
    etc_entry_t **pp = &etc_list;
    while (*pp) {
        if (streq((*pp)->name, name)) {
            etc_entry_t *dead = *pp;
            *pp = dead->next;
            if (dead->data) kfree_heap(dead->data);
            kfree_heap(dead);

            vfs_node_t *etc_dir = _ext4_etc_dir();
            if (etc_dir && etc_dir->ops && etc_dir->ops->delete)
                etc_dir->ops->delete(etc_dir, (char*)name);
            return 0;
        }
        pp = &(*pp)->next;
    }
    return -1;
}

void etcfs_flush(void) {
    for (etc_entry_t *e = etc_list; e; e = e->next) {
        if (e->dirty) {
            _disk_write(e->name, e->data, e->size);
            e->dirty = 0;
        }
    }
}