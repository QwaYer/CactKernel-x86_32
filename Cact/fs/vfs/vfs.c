#include "vfs.h"
#include "klib.h"
#include "sync.h"
#include "kernel.h"
#include "task.h"
#include "memory.h"

// Global VFS root.
vfs_node_t *vfs_root = 0;

// Mount table limits
#define VFS_MOUNT_MAX 32

// Fixed-size symlink pool.
#define VFS_SYMLINK_POOL_SIZE  256
#define VFS_SYMLINK_TARGET_MAX 512

typedef struct {
    vfs_node_t  node;                          // embedded VFS node
    char        target[VFS_SYMLINK_TARGET_MAX];// symlink target path
    int         in_use;                        // allocation flag
} vfs_symlink_entry_t;

static vfs_symlink_entry_t symlink_pool[VFS_SYMLINK_POOL_SIZE];
static mutex_t             symlink_mutex;      // protects symlink_pool

// Mount table entry.
typedef struct {
    vfs_node_t *host;       // mount parent directory
    vfs_node_t *target;     // mounted root node
    char        name[128];  // mount name in parent
} vfs_mount_t;

static vfs_mount_t mount_table[VFS_MOUNT_MAX];
static int         mount_count = 0;

static mutex_t vfs_mutex;   // protects mount_table and symlink_pool

// Look up a mount point by host directory + name
static vfs_node_t *_lookup_mount(vfs_node_t *host, const char *name) {
    for (int i = 0; i < mount_count; i++)
        if (mount_table[i].host == host && streq(mount_table[i].name, name))
            return mount_table[i].target;
    return 0;
}

// Resolve one path segment.
static vfs_node_t *_walk_one(vfs_node_t *dir, const char *name) {
    if (!dir || dir->type != VFS_DIRECTORY) return 0;

    mutex_lock(&vfs_mutex);
    vfs_node_t *m = _lookup_mount(dir, name);
    if (m) vfs_node_ref(m);
    mutex_unlock(&vfs_mutex);
    if (m) return m;

    if (dir->ops && dir->ops->walk)
        return dir->ops->walk(dir, name);
    return 0;
}

// Initialize VFS global state.
void vfs_init(void) {
    mutex_init(&vfs_mutex);
    mutex_init(&symlink_mutex);
    mount_count = 0;
    pr_info("VFS core initialized (mount table + symlinks)");
}

// Mount a filesystem on a host directory
int vfs_mount(vfs_node_t *host, const char *name, vfs_node_t *target) {
    if (!host || !name || !target) return -1;
    mutex_lock(&vfs_mutex);
    if (mount_count >= VFS_MOUNT_MAX) { mutex_unlock(&vfs_mutex); return -1; }
    for (int i = 0; i < mount_count; i++)
        if (mount_table[i].host == host && streq(mount_table[i].name, name)) {
            mutex_unlock(&vfs_mutex);
            return -1;            // duplicate mount
        }
    mount_table[mount_count].host   = host;
    mount_table[mount_count].target = target;
    strlcpy(mount_table[mount_count].name, name, 128);
    mount_count++;
    mutex_unlock(&vfs_mutex);
    return 0;
}

// Unmount a filesystem from a host directory
int vfs_umount(vfs_node_t *host, const char *name) {
    mutex_lock(&vfs_mutex);
    for (int i = 0; i < mount_count; i++) {
        if (mount_table[i].host == host && streq(mount_table[i].name, name)) {
            mount_table[i] = mount_table[--mount_count];  // swap with last
            mutex_unlock(&vfs_mutex);
            return 0;
        }
    }
    mutex_unlock(&vfs_mutex);
    return -1;
}

// Walk path without symlink resolution.
vfs_node_t *vfs_walk_path(vfs_node_t *start, const char *path) {
    if (!path) return start ? start : vfs_root;
    vfs_node_t *cur = start ? start : vfs_root;
    if (!cur) return 0;

    const char *p = path;
    while (*p == '/') p++;

    while (*p && cur) {
        char seg[128];
        int  si = 0;
        while (*p && *p != '/' && si < 127) seg[si++] = *p++;
        if (*p && *p != '/') return 0;
        seg[si] = '\0';
        if (*p == '/') p++;
        if (si == 0) continue;
        if (si == 2 && seg[0] == '.' && seg[1] == '.') continue;
        cur = _walk_one(cur, seg);
    }
    return cur;
}

// Generic VFS I/O wrappers.
int read_vfs(vfs_node_t *node, uint32_t off, uint32_t size, char *buf) {
    if (!node || !node->ops || !node->ops->read) return -1;
    return node->ops->read(node, off, size, buf);
}

int write_vfs(vfs_node_t *node, uint32_t off, uint32_t size, char *buf) {
    if (!node || !node->ops || !node->ops->write) return -1;
    return node->ops->write(node, off, size, buf);
}

void open_vfs(vfs_node_t *node) {
    if (node && node->ops && node->ops->open)
        node->ops->open(node);
}

void close_vfs(vfs_node_t *node) {
    if (node && node->ops && node->ops->close)
        node->ops->close(node);
}

int ioctl_vfs(vfs_node_t *node, uint32_t cmd, void *arg) {
    if (!node || !node->ops || !node->ops->ioctl) return -1;
    return node->ops->ioctl(node, cmd, arg);
}

// Directory operations
vfs_dirent_t *readdir_vfs(vfs_node_t *dir, uint32_t index) {
    if (!dir || dir->type != VFS_DIRECTORY) return 0;
    if (!dir->ops || !dir->ops->readdir)    return 0;
    return dir->ops->readdir(dir, index);
}

vfs_node_t *finddir_vfs(vfs_node_t *dir, char *name) {
    return _walk_one(dir, name);
}

void listdir_vfs(vfs_node_t *dir) {
    if (!dir || dir->type != VFS_DIRECTORY) return;
    if (dir->ops && dir->ops->listdir)
        dir->ops->listdir(dir);
    // Append mount points attached to this directory.
    mutex_lock(&vfs_mutex);
    for (int i = 0; i < mount_count; i++) {
        if (mount_table[i].host == dir) {
            printk("  ");
            printk(mount_table[i].name);
            printk("/\n");
        }
    }
    mutex_unlock(&vfs_mutex);
}

int create_vfs(vfs_node_t *dir, char *name) {
    if (!dir || dir->type != VFS_DIRECTORY || !dir->ops || !dir->ops->create) return -1;
    return dir->ops->create(dir, name);
}

int delete_vfs(vfs_node_t *dir, char *name) {
    if (!dir || dir->type != VFS_DIRECTORY || !dir->ops || !dir->ops->delete) return -1;
    return dir->ops->delete(dir, name);
}

int mkdir_vfs(vfs_node_t *dir, char *name) {
    if (!dir || dir->type != VFS_DIRECTORY || !dir->ops || !dir->ops->mkdir) return -1;
    return dir->ops->mkdir(dir, name);
}

int rmdir_vfs(vfs_node_t *dir, char *name) {
    if (!dir || dir->type != VFS_DIRECTORY || !dir->ops || !dir->ops->rmdir) return -1;
    return dir->ops->rmdir(dir, name);
}

int rename_vfs(vfs_node_t *dir, const char *oldname, const char *newname) {
    if (!dir || dir->type != VFS_DIRECTORY || !dir->ops || !dir->ops->rename) return -1;
    return dir->ops->rename(dir, oldname, newname);
}

// Allocate a symlink node from the static pool
vfs_node_t *vfs_symlink_alloc(const char *target, uint32_t target_len) {
    if (!target) return 0;
    mutex_lock(&symlink_mutex);
    for (int i = 0; i < VFS_SYMLINK_POOL_SIZE; i++) {
        if (!symlink_pool[i].in_use) {
            symlink_pool[i].in_use          = 1;
            symlink_pool[i].node.type       = VFS_SYMLINK;
            symlink_pool[i].node.refcount   = 1;
            symlink_pool[i].node.ops        = 0;
            symlink_pool[i].node.inode      = 0;
            symlink_pool[i].node.name[0]    = '\0';
            uint32_t copy_len = target_len < VFS_SYMLINK_TARGET_MAX - 1
                                ? target_len : VFS_SYMLINK_TARGET_MAX - 1;
            int j;
            for (j = 0; j < (int)copy_len; j++)
                symlink_pool[i].target[j] = target[j];
            symlink_pool[i].target[j]       = '\0';
            symlink_pool[i].node.size       = copy_len;
            symlink_pool[i].node.priv       = symlink_pool[i].target;
            mutex_unlock(&symlink_mutex);
            return &symlink_pool[i].node;
        }
    }
    mutex_unlock(&symlink_mutex);
    return 0;
}

// Read the target of a symlink node
int vfs_readlink_node(vfs_node_t *node, char *buf, uint32_t bufsz) {
    if (!node || node->type != VFS_SYMLINK || !buf || bufsz == 0) return -1;
    if (node->ops && node->ops->readlink)
        return node->ops->readlink(node, buf, bufsz);
    const char *target = (const char *)node->priv;
    if (!target) return -1;
    uint32_t len = 0;
    while (target[len] && len < bufsz - 1) {
        buf[len] = target[len];
        len++;
    }
    buf[len] = '\0';
    return (int)len;
}

// Internal symlink-aware path walk.
static vfs_node_t *_walk_path_follow(vfs_node_t *start, const char *path,
                                      int *depth, int *err);

// Resolve one segment and follow symlinks.
static vfs_node_t *_walk_one_follow(vfs_node_t *dir, const char *seg,
                                     int *depth, int *err) {
    vfs_node_t *node = _walk_one(dir, seg);
    if (!node) return 0;

    if (node->type == VFS_SYMLINK) {
        if (*depth >= VFS_SYMLINK_MAX_DEPTH) {
            *err = ELOOP;
            return 0;
        }
        (*depth)++;

        char target[VFS_SYMLINK_TARGET_MAX];
        int len = vfs_readlink_node(node, target, VFS_SYMLINK_TARGET_MAX);
        if (len <= 0) return 0;

        vfs_node_t *base = (target[0] == '/') ? vfs_root : dir;
        return _walk_path_follow(base, target, depth, err);
    }
    return node;
}

// Walk full path with symlink resolution.
static vfs_node_t *_walk_path_follow(vfs_node_t *start, const char *path,
                                      int *depth, int *err) {
    if (!path) return start ? start : vfs_root;
    vfs_node_t *cur = start ? start : vfs_root;
    if (!cur) return 0;

    const char *p = path;
    while (*p == '/') p++;

    while (*p && cur && !*err) {
        char seg[128];
        int  si = 0;
        while (*p && *p != '/' && si < 127) seg[si++] = *p++;
        if (*p && *p != '/') { *err = ENAMETOOLONG; return 0; }
        seg[si] = '\0';
        if (*p == '/') p++;
        if (si == 0) continue;
        if (si == 2 && seg[0] == '.' && seg[1] == '.') continue;
        cur = _walk_one_follow(cur, seg, depth, err);
    }
    if (*err) return 0;
    return cur;
}

// Public symlink-aware walk API.
vfs_node_t *vfs_walk_path_follow(vfs_node_t *start, const char *path, int *err_out) {
    int depth = 0;
    int err   = 0;
    vfs_node_t *result = _walk_path_follow(start, path, &depth, &err);
    if (err_out) *err_out = err;
    return result;
}

// VFS node refcount helpers.
void vfs_node_ref(vfs_node_t *node) {
    if (node) __sync_fetch_and_add(&node->refcount, 1);
}

void vfs_node_unref(vfs_node_t *node) {
    if (!node) return;
    uint32_t old = __sync_fetch_and_sub(&node->refcount, 1);
    if (old == 0) {
        __sync_fetch_and_add(&node->refcount, 1);
        return;
    }
    if (old == 1 && node->type == VFS_SYMLINK) {
        mutex_lock(&symlink_mutex);
        vfs_symlink_entry_t *entry = (vfs_symlink_entry_t *)node;
        if (entry >= symlink_pool &&
            entry < symlink_pool + VFS_SYMLINK_POOL_SIZE) {
            entry->in_use = 0;
        }
        mutex_unlock(&symlink_mutex);
    }
}

// Create symlink node and mount it under parent.
int vfs_symlink(vfs_node_t *dir, const char *name, const char *target) {
    if (!dir || dir->type != VFS_DIRECTORY || !name || !target) return -1;

    // Delegate when filesystem provides native symlink support.
    if (dir->ops && dir->ops->symlink)
        return dir->ops->symlink(dir, name, target);

    int tlen = 0;
    while (target[tlen]) tlen++;

    vfs_node_t *sym = vfs_symlink_alloc(target, (uint32_t)tlen);
    if (!sym) return -1;
    strlcpy(sym->name, name, 128);

    int ret = vfs_mount(dir, name, sym);
    if (ret != 0)
        vfs_node_unref(sym);
    return ret;
}

// Create a hard link
int vfs_link(vfs_node_t *dir, const char *name, vfs_node_t *target_node) {
    if (!dir || dir->type != VFS_DIRECTORY || !name || !target_node) return -1;
    if (!dir->ops || !dir->ops->link) return -1;
    int ret = dir->ops->link(dir, name, target_node);
    if (ret == 0) vfs_node_ref(target_node);
    return ret;
}

// Unlink a file or symlink by name
int vfs_unlink(vfs_node_t *dir, const char *name) {
    if (!dir || dir->type != VFS_DIRECTORY || !name) return -1;

    // Check if it's a mounted symlink
    mutex_lock(&vfs_mutex);
    vfs_node_t *mounted = 0;
    for (int i = 0; i < mount_count; i++) {
        if (mount_table[i].host == dir && streq(mount_table[i].name, name)) {
            mounted = mount_table[i].target;
            if (mounted) vfs_node_ref(mounted);
            break;
        }
    }
    mutex_unlock(&vfs_mutex);

    if (mounted && mounted->type == VFS_SYMLINK) {
        int ret = vfs_umount(dir, name);
        vfs_node_unref(mounted);
        return ret;
    }
    if (mounted) vfs_node_unref(mounted);

    // Delegate to the underlying filesystem.
    // NOTE: finddir_vfs acquires vfs_mutex internally, so we cannot
    // extend the mutex across finddir+unlink (would deadlock).  In a
    // single-CPU non-preemptive kernel the TOCTOU window between the
    // lookup and the removal is not practically exploitable.
    if (dir->ops && dir->ops->unlink) {
        vfs_node_t *node = finddir_vfs(dir, (char *)name);
        int ret = dir->ops->unlink(dir, name);
        if (ret == 0 && node) vfs_node_unref(node);
        return ret;
    }
    if (dir->ops && dir->ops->delete)
        return dir->ops->delete(dir, name);
    return -1;
}

// POSIX rwx permission check.
// NOTE: This is a TOCTOU window — the node's mode/uid/gid or the current
// task's credentials could change between the check and the VFS operation.
// Callers should hold vfs_mutex (or equivalent) across check + operation.
int vfs_check_perm(vfs_node_t *node, uint32_t perm) {
    if (!node) return -1;

    // No mode set → allow everything
    if (node->mode == 0) return 0;

    // Kernel tasks bypass permission checks
    if (!current_task || current_task->is_kernel) return 0;

    // Root (euid=0) bypasses permission checks
    if (current_task->proc && current_task->proc->euid == 0) return 0;

    uint32_t shift;
    if (current_task->proc && current_task->proc->euid == node->uid)
        shift = 6;         // owner
    else if (current_task->proc && current_task->proc->egid == node->gid)
        shift = 3;         // group
    else
        shift = 0;         // other

    uint32_t allowed = (node->mode >> shift) & 0x07;
    if ((allowed & perm) == perm)
        return 0;
    return -1;
}

// ── File descriptor management ──────────────────────────────────────────

file_t *file_alloc(vfs_node_t *node) {
    if (!node) return 0;
    file_t *f = (file_t *)kmalloc(sizeof(file_t));
    if (!f) return 0;
    f->node     = node;
    f->offset   = 0;
    f->flags    = 0;
    f->cloexec  = 0;
    f->refcount = 1;
    open_vfs(node);
    return f;
}

void file_free(file_t *f) {
    if (!f) return;
    close_vfs(f->node);
    kfree(f);
}

file_t *file_ref(file_t *f) {
    if (f) f->refcount++;
    return f;
}

int file_unref(file_t *f) {
    if (!f) return -1;
    if (f->refcount == 0) return -1;
    f->refcount--;
    if (f->refcount == 0) {
        file_free(f);
        return 0;
    }
    return f->refcount;
}

// ── Path resolution (moved from syscall layer) ───────────────────────────

void vfs_make_abs(const char *path, char *abs, int abs_max) {
    int p = 0;
    if (path[0] != '/') {
        for (int i = 0; current_task->proc->cwd[i] && p < abs_max - 2; i++)
            abs[p++] = current_task->proc->cwd[i];
        if (p > 0 && abs[p-1] != '/') abs[p++] = '/';
    }
    for (int i = 0; path[i] && p < abs_max - 1; i++)
        abs[p++] = path[i];
    abs[p] = '\0';
}

vfs_node_t *vfs_resolve_path(const char *path) {
    if (!path || !current_task) return 0;
    char abs[512];
    vfs_make_abs(path, abs, 512);
    return vfs_walk_path_follow(vfs_root, abs, 0);
}

vfs_node_t *vfs_resolve_parent_follow(const char *path,
                                       char *basename_out, int basename_max) {
    if (!path || !current_task) return 0;

    char abs[512];
    vfs_make_abs(path, abs, 512);

    int last_slash = -1;
    for (int i = 0; abs[i]; i++)
        if (abs[i] == '/') last_slash = i;

    if (last_slash < 0) {
        int i = 0;
        while (path[i] && i < basename_max - 1) { basename_out[i] = path[i]; i++; }
        basename_out[i] = '\0';
        return vfs_walk_path_follow(vfs_root, current_task->proc->cwd, 0);
    }

    const char *bn = abs + last_slash + 1;
    int i = 0;
    while (bn[i] && i < basename_max - 1) { basename_out[i] = bn[i]; i++; }
    basename_out[i] = '\0';

    if (last_slash == 0) return vfs_root;

    char parent_path[512];
    for (int j = 0; j < last_slash && j < 511; j++)
        parent_path[j] = abs[j];
    parent_path[last_slash] = '\0';

    return vfs_walk_path_follow(vfs_root, parent_path, 0);
}

vfs_node_t *vfs_resolve_parent(const char *path,
                                char *basename_out, int basename_max) {
    return vfs_resolve_parent_follow(path, basename_out, basename_max);
}

// ── New VFS wrapper functions ───────────────────────────────────────────

int truncate_vfs(vfs_node_t *node, uint32_t length) {
    if (!node) return -1;
    if (node->ops && node->ops->truncate)
        return node->ops->truncate(node, length);
    node->size = length;
    return 0;
}

int chmod_vfs(vfs_node_t *node, uint32_t mode) {
    if (!node) return -1;
    if (node->ops && node->ops->chmod)
        return node->ops->chmod(node, mode);
    node->mode = mode & 0777;
    return 0;
}

int chown_vfs(vfs_node_t *node, uint32_t uid, uint32_t gid) {
    if (!node) return -1;
    if (node->ops && node->ops->chown)
        return node->ops->chown(node, uid, gid);
    if (uid != (uint32_t)-1) node->uid = uid;
    if (gid != (uint32_t)-1) node->gid = gid;
    return 0;
}

int mknod_vfs(vfs_node_t *dir, const char *name, uint32_t mode, uint32_t dev) {
    (void)dev;
    if (!dir || dir->type != VFS_DIRECTORY || !name) return -1;
    if (dir->ops && dir->ops->mknod)
        return dir->ops->mknod(dir, name, mode, dev);
    if (!dir->ops || !dir->ops->create) return -1;
    int ret = dir->ops->create(dir, (char *)name);
    if (ret < 0) return -1;
    vfs_node_t *node = finddir_vfs(dir, (char *)name);
    if (!node) return -1;
    if ((mode & 0xF000) == 0x2000)
        node->type = VFS_CHARDEVICE;
    else if ((mode & 0xF000) == 0x6000)
        node->type = VFS_BLOCKDEVICE;
    node->mode = mode & 0777;
    return 0;
}

int stat_vfs(vfs_node_t *node, uint32_t *buf) {
    if (!node || !buf) return -1;
    if (node->ops && node->ops->stat)
        return node->ops->stat(node, buf);
    vfs_fill_stat(node, buf);
    return 0;
}

int poll_vfs(vfs_node_t *node, uint32_t events) {
    if (!node) return VFS_POLLNVAL;
    if (node->ops && node->ops->poll)
        return node->ops->poll(node, events);
    // Default: regular files and dirs are always ready
    uint32_t revents = 0;
    if (events & VFS_POLLIN)  revents |= VFS_POLLIN;
    if (events & VFS_POLLOUT) revents |= VFS_POLLOUT;
    return (int)revents;
}

int lseek_vfs(vfs_node_t *node, int offset, int whence, uint32_t *result) {
    if (!node || !result) return -1;
    if (node->ops && node->ops->lseek)
        return node->ops->lseek(node, offset, whence, result);
    // Default: files always support seek
    return -1;  // caller handles default lseek logic
}

// ── Helper functions (moved from syscall helper.c) ──────────────────────

uint32_t vfs_type_to_mode(uint32_t type) {
    switch (type) {
    case VFS_FILE:        return 0x8000;   // S_IFREG
    case VFS_DIRECTORY:   return 0x4000;   // S_IFDIR
    case VFS_CHARDEVICE:  return 0x2000;   // S_IFCHR
    case VFS_BLOCKDEVICE: return 0x6000;   // S_IFBLK
    case VFS_PIPE:        return 0x1000;   // S_IFIFO
    default:              return 0;
    }
}

void vfs_fill_stat(vfs_node_t *node, uint32_t *buf) {
    buf[0] = node->inode;
    buf[1] = vfs_type_to_mode(node->type);
    buf[2] = node->size;
    buf[3] = node->type;
}

void vfs_strlcpy(char *dst, const char *src, int max) {
    int i = 0;
    while (src[i] && i < max - 1) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}