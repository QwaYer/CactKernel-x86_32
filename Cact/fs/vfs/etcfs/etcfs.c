#include "etcfs.h"
#include "vfs.h"
#include "kernel.h"
#include "memory.h"
#include "klib.h"

// Internal entry descriptor — one per file in /etc
typedef struct etc_entry etc_entry_t;

struct etc_entry {
    char          name[ETCFS_NAME_LEN];
    char         *data;       // file contents (heap-allocated, cap-sized)
    uint32_t      size;       // current data length
    uint32_t      cap;        // allocated capacity
    int           dirty;      // writeback pending
    vfs_node_t    node;       // VFS node for this file
    etc_entry_t  *next;
};

// Global state
static etc_entry_t *etc_list   = 0;
static vfs_node_t   etc_root;
static vfs_node_t  *ext4_root  = 0;    // bound ext4 root for disk I/O
static int          etcfs_ready = 0;

// Find an entry by name in the linked list
static etc_entry_t *_find(const char *name) {
    for (etc_entry_t *e = etc_list; e; e = e->next)
        if (streq(e->name, name)) return e;
    return 0;
}

// Resolve ext4 /etc directory (lazy, on first use)
static vfs_node_t *_ext4_etc_dir(void) {
    if (!ext4_root || !ext4_root->ops || !ext4_root->ops->walk) return 0;
    return ext4_root->ops->walk(ext4_root, "etc");
}

// Read a file from ext4 /etc/<name> into buf (up to maxsize bytes)
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

// Write a file to ext4 /etc/<name> (create/mkdir /etc if needed)
static int _disk_write(const char *name, const char *buf, uint32_t size) {
    if (!ext4_root) return -1;

    vfs_node_t *etc_dir = _ext4_etc_dir();
    if (!etc_dir) {
        if (!ext4_root->ops || !ext4_root->ops->mkdir) return -1;
        if (ext4_root->ops->mkdir(ext4_root, "etc") < 0) return -1;
        etc_dir = _ext4_etc_dir();
        if (!etc_dir) return -1;
    }

    // Remove existing file before creating a new one
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

// Grow entry capacity to at least 'needed' bytes (doubling strategy)
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

// VFS file ops: read/write from in-memory entry with disk writeback
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

// etcfs root directory ops
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

// Load all files from ext4 /etc into memory on first init
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
        if (_find(de->name)) continue;   // already loaded

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
    }
}

// Return the etcfs root VFS node (to be registered in mount table)
vfs_node_t *etcfs_get_root(void) { return &etc_root; }

// Initialise etcfs, bind to ext4 root, load files, seed users
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

    etcfs_seed_users();
}

// Read a file from etcfs by name; returns bytes read, 0 if not found
int etcfs_read(const char *name, char *buf, uint32_t size) {
    etc_entry_t *e = _find(name);
    if (!e || !e->data) return 0;
    uint32_t sz = e->size < size ? e->size : size;
    memcpy(buf, e->data, sz);
    return (int)sz;
}

// Write a file to etcfs by name; creates if not exists
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

// Create an empty file in etcfs (no-op if already exists)
int etcfs_create(const char *name) {
    if (_find(name)) return 0;   // already exists

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

// Delete a file from etcfs by name; removes from disk as well
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

// Flush all dirty entries to disk
void etcfs_flush(void) {
    for (etc_entry_t *e = etc_list; e; e = e->next) {
        if (e->dirty) {
            _disk_write(e->name, e->data, e->size);
            e->dirty = 0;
        }
    }
}

// Default /etc/passwd if none exists
static const char default_passwd[] =
    "root:x:0:0:root:/:/shell\n"
    "daemon:x:1:1:daemon:/:/shell\n"
    "nobody:x:65534:65534:nobody:/:/shell\n";

// Default /etc/group if none exists
static const char default_group[] =
    "root:x:0:\n"
    "daemon:x:1:\n"
    "nogroup:x:65534:\n";

// Seed default passwd and group files if they don't exist
void etcfs_seed_users(void) {
    char tmp[8];
    if (etcfs_read("passwd", tmp, 4) <= 0)
        etcfs_write("passwd", default_passwd, sizeof(default_passwd) - 1);
    if (etcfs_read("group", tmp, 4) <= 0)
        etcfs_write("group", default_group, sizeof(default_group) - 1);
}

// Parse an unsigned integer from a string, advancing the pointer
static uint32_t _parse_uint(const char *s, const char **end) {
    uint32_t v = 0;
    while (*s >= '0' && *s <= '9') { v = v * 10 + (uint32_t)(*s - '0'); s++; }
    if (end) *end = s;
    return v;
}

static char _uid_name_buf[64];

// Look up a user name by UID from /etc/passwd
const char *etcfs_uid_to_name(uint32_t uid) {
    char raw[1024];
    int n = etcfs_read("passwd", raw, sizeof(raw) - 1);
    if (n <= 0) { _uid_name_buf[0] = '?'; _uid_name_buf[1] = '\0'; return _uid_name_buf; }
    raw[n] = '\0';

    const char *p = raw;
    while (*p) {
        const char *name_start = p;
        while (*p && *p != ':') p++;
        int name_len = (int)(p - name_start);
        if (*p == ':') p++;   // skip passwd field
        while (*p && *p != ':') p++;
        if (*p == ':') p++;
        uint32_t file_uid = _parse_uint(p, &p);
        if (file_uid == uid) {
            if (name_len > 63) name_len = 63;
            for (int i = 0; i < name_len; i++) _uid_name_buf[i] = name_start[i];
            _uid_name_buf[name_len] = '\0';
            return _uid_name_buf;
        }
        while (*p && *p != '\n') p++;
        if (*p == '\n') p++;
    }
    _uid_name_buf[0] = '?'; _uid_name_buf[1] = '\0';
    return _uid_name_buf;
}

// Look up a UID by user name from /etc/passwd
uint32_t etcfs_name_to_uid(const char *name) {
    char raw[1024];
    int n = etcfs_read("passwd", raw, sizeof(raw) - 1);
    if (n <= 0) return (uint32_t)-1;
    raw[n] = '\0';

    int target_len = 0;
    while (name[target_len]) target_len++;

    const char *p = raw;
    while (*p) {
        const char *name_start = p;
        while (*p && *p != ':') p++;
        int nlen = (int)(p - name_start);
        if (*p == ':') p++;
        while (*p && *p != ':') p++;
        if (*p == ':') p++;
        uint32_t uid = _parse_uint(p, &p);

        if (nlen == target_len) {
            int match = 1;
            for (int i = 0; i < nlen; i++)
                if (name_start[i] != name[i]) { match = 0; break; }
            if (match) return uid;
        }
        while (*p && *p != '\n') p++;
        if (*p == '\n') p++;
    }
    return (uint32_t)-1;
}

// Look up a GID by group name from /etc/group
uint32_t etcfs_name_to_gid(const char *name) {
    char raw[1024];
    int n = etcfs_read("group", raw, sizeof(raw) - 1);
    if (n <= 0) return (uint32_t)-1;
    raw[n] = '\0';

    int target_len = 0;
    while (name[target_len]) target_len++;

    const char *p = raw;
    while (*p) {
        const char *name_start = p;
        while (*p && *p != ':') p++;
        int nlen = (int)(p - name_start);
        if (*p == ':') p++;
        while (*p && *p != ':') p++;
        if (*p == ':') p++;
        uint32_t gid = _parse_uint(p, &p);

        if (nlen == target_len) {
            int match = 1;
            for (int i = 0; i < nlen; i++)
                if (name_start[i] != name[i]) { match = 0; break; }
            if (match) return gid;
        }
        while (*p && *p != '\n') p++;
        if (*p == '\n') p++;
    }
    return (uint32_t)-1;
}