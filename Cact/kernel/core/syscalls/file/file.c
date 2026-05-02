#include "file.h"
#include "validate.h"
#include "resolve.h"
#include "helper.h"

// stat() — get file metadata by path
int sys_stat(struct syscall_frame* regs) {
    char*     path = (char*)regs->ebx;
    uint32_t* ubuf = (uint32_t*)regs->ecx;

    if (!current_task) return -1;
    if (!validate_user_str(path)) return -1;
    if (!validate_user_ptr(ubuf, 16)) return -1;

    struct vfs_node* node = vfs_walk_path(vfs_root, path);
    if (!node) {
        kprint("[DBG] sys_stat: not found: "); kprint(path); kprint("\n");
        return -1;
    }

    _fill_stat(node, ubuf);

    kprint("[DBG] sys_stat: "); kprint(path);
    kprint(" type=");
    char tmp[16]; itoa((int)node->type, tmp); kprint(tmp);
    kprint(" size="); itoa((int)node->size, tmp); kprint(tmp);
    kprint("\n");

    return 0;
}

// fstat() — get file metadata by fd
int sys_fstat(struct syscall_frame* regs) {
    int       fd   = (int)regs->ebx;
    uint32_t* ubuf = (uint32_t*)regs->ecx;

    if (!current_task) return -1;
    if (fd < 0 || fd >= MAX_FD) return -1;
    if (!validate_user_ptr(ubuf, 16)) return -1;

    struct vfs_node* node = current_task->fds->fd_table[fd];
    if (!node) {
        kprint("[DBG] sys_fstat: bad fd=");
        char tmp[16]; itoa(fd, tmp); kprint(tmp); kprint("\n");
        return -1;
    }

    _fill_stat(node, ubuf);

    kprint("[DBG] sys_fstat: fd=");
    char tmp[16]; itoa(fd, tmp); kprint(tmp);
    kprint(" type="); itoa((int)node->type, tmp); kprint(tmp);
    kprint(" size="); itoa((int)node->size, tmp); kprint(tmp);
    kprint("\n");

    return 0;
}

// access() — check file permissions
int sys_access(char* path, int mode) {
    if (!validate_user_str(path)) return -1;
    if (!current_task) return -1;
    vfs_node_t* node = _resolve_path(path);
    if (!node) return -1;
    if (mode == 0) return 0;   // F_OK: just check existence
    uint32_t perm = 0;
    if (mode & 4) perm |= VFS_PERM_READ;
    if (mode & 2) perm |= VFS_PERM_WRITE;
    if (mode & 1) perm |= VFS_PERM_EXEC;
    return vfs_check_perm(node, perm);
}

// chmod() — change file mode (owner or root only)
int sys_chmod(char* path, uint32_t mode) {
    if (!validate_user_str(path)) return -1;
    if (!current_task) return -1;

    vfs_node_t* node = _resolve_path(path);
    if (!node) return -1;

    // Only owner or root can change mode
    if (current_task->euid != 0 && current_task->euid != node->uid)
        return -1;

    node->mode = mode & 0777;
    return 0;
}

// chown() — change file owner/group (root only)
int sys_chown(char* path, uint32_t new_uid, uint32_t new_gid) {
    if (!validate_user_str(path)) return -1;
    if (!current_task) return -1;

    vfs_node_t* node = _resolve_path(path);
    if (!node) return -1;

    // Only root can change ownership
    if (current_task->euid != 0) return -1;

    if (new_uid != (uint32_t)-1) node->uid = new_uid;
    if (new_gid != (uint32_t)-1) node->gid = new_gid;
    return 0;
}

// umask() — set file creation mask
int sys_umask(uint32_t mask) {
    if (!current_task) return -1;
    uint32_t old = current_task->umask;
    current_task->umask = mask & 0777;
    return (int)old;
}

// truncate() — truncate file to given length by path
int sys_truncate(char* path, uint32_t length) {
    if (!validate_user_str(path)) return -1;
    if (!current_task) return -1;
    vfs_node_t* node = _resolve_path(path);
    if (!node || node->type != VFS_FILE) return -1;
    if (vfs_check_perm(node, VFS_PERM_WRITE) < 0) return -1;
    node->size = length;
    return 0;
}

// ftruncate() — truncate file to given length by fd
int sys_ftruncate(int fd, uint32_t length) {
    if (!current_task) return -1;
    if (fd < 0 || fd >= MAX_FD) return -1;
    vfs_node_t* node = current_task->fds->fd_table[fd];
    if (!node || node->type != VFS_FILE) return -1;
    node->size = length;
    return 0;
}

// sync() — flush all filesystem buffers (no-op for now)
int sys_sync(void) {
    return 0;
}

// fsync() — flush a single file descriptor (no-op for now)
int sys_fsync(int fd) {
    (void)fd;
    return 0;
}

// mknod() — create a device node (character or block)
int sys_mknod(char* path, uint32_t mode, uint32_t dev) {
    (void)dev;
    if (!validate_user_str(path)) return -1;
    if (!current_task) return -1;
    char basename[128];
    vfs_node_t* parent = _resolve_parent(path, basename, 128);
    if (!parent || !basename[0]) return -1;
    int ret = create_vfs(parent, basename);
    if (ret < 0) return -1;
    vfs_node_t* node = finddir_vfs(parent, basename);
    if (!node) return -1;
    if ((mode & 0xF000) == 0x2000)
        node->type = VFS_CHARDEVICE;
    else if ((mode & 0xF000) == 0x6000)
        node->type = VFS_BLOCKDEVICE;
    node->mode = mode & 0777;
    return 0;
}