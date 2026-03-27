#include "vfs.h"
#include "libc.h"
#include "sync.h"
#include "kernel.h"

vfs_node_t *vfs_root = 0;

#define VFS_MOUNT_MAX 32

typedef struct {
    vfs_node_t *host;
    vfs_node_t *target;
    char        name[128];
} vfs_mount_t;

static vfs_mount_t mount_table[VFS_MOUNT_MAX];
static int         mount_count = 0;

static mutex_t vfs_mutex;



static vfs_node_t *_lookup_mount(vfs_node_t *host, const char *name) {
    for (int i = 0; i < mount_count; i++)
        if (mount_table[i].host == host && streq(mount_table[i].name, name))
            return mount_table[i].target;
    return 0;
}

static vfs_node_t *_walk_one(vfs_node_t *dir, const char *name) {
    if (!dir || dir->type != VFS_DIRECTORY) return 0;

    mutex_lock(&vfs_mutex);
    vfs_node_t *m = _lookup_mount(dir, name);
    mutex_unlock(&vfs_mutex);
    if (m) return m;

    if (dir->ops && dir->ops->walk)
        return dir->ops->walk(dir, name);
    return 0;
}


//Public api
void vfs_init(void) {
    mutex_init(&vfs_mutex);
    mount_count = 0;
}

int vfs_mount(vfs_node_t *host, const char *name, vfs_node_t *target) {
    if (!host || !name || !target) return -1;
    mutex_lock(&vfs_mutex);
    if (mount_count >= VFS_MOUNT_MAX) { mutex_unlock(&vfs_mutex); return -1; }
    for (int i = 0; i < mount_count; i++)
        if (mount_table[i].host == host && streq(mount_table[i].name, name)) {
            mutex_unlock(&vfs_mutex);
            return -1;
        }
    mount_table[mount_count].host   = host;
    mount_table[mount_count].target = target;
    strlcpy(mount_table[mount_count].name, name, 128);
    mount_count++;
    mutex_unlock(&vfs_mutex);
    return 0;
}

int vfs_umount(vfs_node_t *host, const char *name) {
    mutex_lock(&vfs_mutex);
    for (int i = 0; i < mount_count; i++) {
        if (mount_table[i].host == host && streq(mount_table[i].name, name)) {
            mount_table[i] = mount_table[--mount_count];
            mutex_unlock(&vfs_mutex);
            return 0;
        }
    }
    mutex_unlock(&vfs_mutex);
    return -1;
}

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
        seg[si] = '\0';
        if (*p == '/') p++;
        if (si == 0) continue;
        cur = _walk_one(cur, seg);
    }
    return cur;
}


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
    mutex_lock(&vfs_mutex);
    for (int i = 0; i < mount_count; i++) {
        if (mount_table[i].host == dir) {
            kprint("  ");
            kprint(mount_table[i].name);
            kprint("/\n");
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