#include "file.h"
#include "validate.h"
#include "memory.h"

static inline file_t *_get_file(int fd) {
    if (!current_task) return 0;
    if (fd < 0 || fd >= MAX_FD) return 0;
    return current_task->proc->fds->files[fd];
}

int sys_stat(struct syscall_frame *regs) {
    char      *path = (char *)regs->ebx;
    uint32_t  *ubuf = (uint32_t *)regs->ecx;

    if (!current_task) return -1;
    if (!validate_user_ptr(ubuf, 16)) return -1;
    char *kpath = copy_path_from_user(path);
    if (!kpath) return -1;

    vfs_node_t *node = _resolve_path(kpath);
    kfree(kpath);
    if (!node) return -1;

    return stat_vfs(node, ubuf);
}

int sys_access(char *path, int mode) {
    if (!current_task) return -1;
    char *kpath = copy_path_from_user(path);
    if (!kpath) return -1;

    vfs_node_t *node = _resolve_path(kpath);
    kfree(kpath);
    if (!node) return -1;
    if (mode == 0) return 0;
    uint32_t perm = 0;
    if (mode & 4) perm |= VFS_PERM_READ;
    if (mode & 2) perm |= VFS_PERM_WRITE;
    if (mode & 1) perm |= VFS_PERM_EXEC;
    return vfs_check_perm(node, perm);
}

int sys_chmod(char *path, uint32_t mode) {
    if (!current_task) return -1;
    char *kpath = copy_path_from_user(path);
    if (!kpath) return -1;

    vfs_node_t *node = _resolve_path(kpath);
    kfree(kpath);
    if (!node) return -1;

    if (current_task->proc->euid != 0 && current_task->proc->euid != node->uid)
        return -1;

    return chmod_vfs(node, mode);
}

int sys_chown(char *path, uint32_t new_uid, uint32_t new_gid) {
    if (!current_task) return -1;
    char *kpath = copy_path_from_user(path);
    if (!kpath) return -1;

    vfs_node_t *node = _resolve_path(kpath);
    kfree(kpath);
    if (!node) return -1;

    if (current_task->proc->euid != 0) return -1;

    return chown_vfs(node, new_uid, new_gid);
}

int sys_truncate(char *path, uint32_t length) {
    if (!current_task) return -1;
    char *kpath = copy_path_from_user(path);
    if (!kpath) return -1;

    vfs_node_t *node = _resolve_path(kpath);
    kfree(kpath);
    if (!node || node->type != VFS_FILE) return -1;
    if (vfs_check_perm(node, VFS_PERM_WRITE) < 0) return -1;
    return truncate_vfs(node, length);
}

int sys_mknod(char *path, uint32_t mode, uint32_t dev) {
    if (!current_task) return -1;
    char *kpath = copy_path_from_user(path);
    if (!kpath) return -1;

    char basename[128];
    vfs_node_t *parent = _resolve_parent(kpath, basename, 128);
    kfree(kpath);
    if (!parent || !basename[0]) return -1;
    return mknod_vfs(parent, basename, mode, dev);
}

int sys_fstat(struct syscall_frame *regs) {
    int       fd   = (int)regs->ebx;
    uint32_t *ubuf = (uint32_t *)regs->ecx;

    if (!current_task) return -1;
    if (fd < 0 || fd >= MAX_FD) return -1;
    if (!validate_user_ptr(ubuf, 16)) return -1;

    file_t *f = _get_file(fd);
    if (!f || !f->node) return -1;

    return stat_vfs(f->node, ubuf);
}

int sys_ftruncate(int fd, uint32_t length) {
    file_t *f = _get_file(fd);
    if (!f || !f->node) return -1;
    if (f->node->type != VFS_FILE) return -1;
    return truncate_vfs(f->node, length);
}

int sys_umask(uint32_t mask) {
    if (!current_task) return -1;
    uint32_t old = current_task->proc->umask;
    current_task->proc->umask = mask & 0777;
    return (int)old;
}

int sys_sync(void) { return 0; }
int sys_fsync(int fd) { (void)fd; return 0; }
