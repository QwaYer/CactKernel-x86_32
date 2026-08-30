#include "vfs.h"
#include "vfs_internal.h"
#include "klib.h"
#include "sync.h"
#include "kernel.h"
#include "task.h"
#include "memory.h"

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
