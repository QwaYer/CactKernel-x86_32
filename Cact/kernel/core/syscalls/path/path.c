#include "path.h"
#include "validate.h"
#include "resolve.h"

// create() — create a regular file in a directory
int sys_create(char* name) {
    if (!validate_user_str(name)) return -1;
    if (!current_task) return -1;

    char basename[128];
    vfs_node_t* parent = _resolve_parent(name, basename, 128);
    if (!parent) {
        return -1;
    }
    if (!basename[0]) {
        return -1;
    }

    return create_vfs(parent, basename);
}

// mkdir() — create a new directory
int sys_mkdir(char* pathname) {
    if (!validate_user_str(pathname)) return -1;
    if (!current_task) return -1;

    char basename[128];
    vfs_node_t* parent = _resolve_parent(pathname, basename, 128);
    if (!parent) {
        return -1;
    }
    if (!basename[0]) {
        return -1;
    }

    // Need write + exec on parent to create entries
    if (vfs_check_perm(parent, VFS_PERM_WRITE | VFS_PERM_EXEC) < 0) return -1;

    return mkdir_vfs(parent, basename);
}

// rmdir() — remove an empty directory
int sys_rmdir(char* pathname) {
    if (!validate_user_str(pathname)) return -1;
    if (!current_task) return -1;

    char basename[128];
    vfs_node_t* parent = _resolve_parent(pathname, basename, 128);
    if (!parent) {
        return -1;
    }
    if (!basename[0]) {
        return -1;
    }
    if (vfs_check_perm(parent, VFS_PERM_WRITE | VFS_PERM_EXEC) < 0) return -1;

    return rmdir_vfs(parent, basename);
}

// delete() — remove a file (legacy name, same as unlink)
int sys_delete(char* name) {
    if (!validate_user_str(name)) return -1;
    if (!current_task) return -1;

    char basename[128];
    vfs_node_t* parent = _resolve_parent(name, basename, 128);
    if (!parent) {
        return -1;
    }
    if (!basename[0]) return -1;

    return delete_vfs(parent, basename);
}

// unlink() — remove a file or symlink by path (following symlinks)
int sys_unlink(char* path) {
    if (!validate_user_str(path)) return -1;
    if (!current_task) return -1;

    char basename[128];
    vfs_node_t* parent = _resolve_parent_follow(path, basename, 128);
    if (!parent || !basename[0]) return -1;
    if (vfs_check_perm(parent, VFS_PERM_WRITE | VFS_PERM_EXEC) < 0) return -1;

    return vfs_unlink(parent, basename);
}

// rename() — rename a file or directory within the same parent
int sys_rename(char* oldpath, char* newpath) {
    if (!validate_user_str(oldpath)) return -1;
    if (!validate_user_str(newpath)) return -1;
    if (!current_task) return -1;

    char old_base[128], new_base[128];
    vfs_node_t* old_parent = _resolve_parent(oldpath, old_base, 128);
    vfs_node_t* new_parent = _resolve_parent(newpath, new_base, 128);

    if (!old_parent || !new_parent) return -1;
    if (!old_base[0] || !new_base[0]) return -1;

    // Only rename within the same directory for now
    if (old_parent != new_parent) return -1;

    return rename_vfs(old_parent, old_base, new_base);
}

// link() — create a hard link
int sys_link(struct syscall_frame* regs) {
    char* oldpath = (char*)regs->ebx;
    char* newpath = (char*)regs->ecx;
    if (!validate_user_str(oldpath)) return -1;
    if (!validate_user_str(newpath)) return -1;
    if (!current_task) return -1;

    vfs_node_t* target_node = _resolve_path(oldpath);
    if (!target_node) return -1;

    char basename[128];
    vfs_node_t* new_parent = _resolve_parent_follow(newpath, basename, 128);
    if (!new_parent || !basename[0]) return -1;

    return vfs_link(new_parent, basename, target_node);
}

// symlink() — create a symbolic link
int sys_symlink(struct syscall_frame* regs) {
    char* target   = (char*)regs->ebx;
    char* linkpath = (char*)regs->ecx;
    if (!validate_user_str(target))   return -1;
    if (!validate_user_str(linkpath)) return -1;
    if (!current_task) return -1;

    char basename[128];
    vfs_node_t* parent = _resolve_parent_follow(linkpath, basename, 128);
    if (!parent || !basename[0]) return -1;

    return vfs_symlink(parent, basename, target);
}

// readlink() — read the target of a symbolic link
int sys_readlink(struct syscall_frame* regs) {
    char*    path  = (char*)regs->ebx;
    char*    buf   = (char*)regs->ecx;
    uint32_t bufsz = regs->edx;

    if (!validate_user_str(path))       return -1;
    if (!validate_user_ptr(buf, bufsz)) return -1;
    if (bufsz == 0)                     return -1;
    if (!current_task)                  return -1;

    char basename[128];
    vfs_node_t* parent = _resolve_parent_follow(path, basename, 128);
    if (!parent || !basename[0]) return -1;

    vfs_node_t* node = finddir_vfs(parent, basename);
    if (!node) return -1;
    if (node->type != VFS_SYMLINK) return -1;

    return vfs_readlink_node(node, buf, bufsz);
}

// getdents() — read directory entries into a user buffer
int sys_getdents(struct syscall_frame* regs) {
    int      fd    = (int)regs->ebx;
    char*    buf   = (char*)regs->ecx;
    uint32_t count = regs->edx;

    if (!current_task) return -1;
    if (fd < 0 || fd >= MAX_FD) return -1;

    struct vfs_node* node = current_task->fds->fd_table[fd];
    if (!node) return -1;
    if (node->type != VFS_DIRECTORY) return -1;

    uint32_t entry_size = sizeof(struct cact_dirent);
    if (!validate_user_ptr(buf, count)) return -1;
    if (count < entry_size) return -1;

    uint32_t written = 0;
    uint32_t index   = current_task->fds->fd_offset[fd];

    while (written + entry_size <= count) {
        struct vfs_dirent* de = readdir_vfs(node, index);
        if (!de) break;

        struct cact_dirent* out = (struct cact_dirent*)(buf + written);
        out->d_ino = de->inode;

        int i = 0;
        while (de->name[i] && i < 123) { out->d_name[i] = de->name[i]; i++; }
        out->d_name[i] = '\0';

        written += entry_size;
        index++;
    }

    current_task->fds->fd_offset[fd] = index;
    return (int)written;
}

// chdir() — change the current working directory
int sys_chdir(struct syscall_frame* regs) {
    char* path = (char*)regs->ebx;

    if (!current_task) return -1;
    if (!validate_user_str(path)) return -1;

    // Build absolute path from current directory + given path
    char abs[256];
    if (path[0] == '/') {
        int i = 0;
        while (path[i] && i < 255) { abs[i] = path[i]; i++; }
        abs[i] = '\0';
    } else {
        int p = 0;
        for (int i = 0; current_task->cwd[i] && p < 254; i++)
            abs[p++] = current_task->cwd[i];
        if (p > 0 && abs[p-1] != '/') abs[p++] = '/';
        for (int i = 0; path[i] && p < 255; i++)
            abs[p++] = path[i];
        abs[p] = '\0';
    }

    // Normalise: resolve . and .. components without touching the filesystem
    int segs_start[64], segs_len[64];
    int nseg = 0;
    const char* s = abs;
    while (*s) {
        while (*s == '/') s++;
        if (!*s) break;
        const char* seg = s;
        int slen = 0;
        while (*s && *s != '/') { s++; slen++; }
        if (slen == 1 && seg[0] == '.')
            continue;                          // skip "."
        if (slen == 2 && seg[0] == '.' && seg[1] == '.') {
            if (nseg > 0) nseg--;               // pop for ".."
            continue;
        }
        segs_start[nseg] = (int)(seg - abs);
        segs_len[nseg]   = slen;
        nseg++;
        if (nseg >= 64) break;
    }

    char norm[256];
    if (nseg == 0) {
        norm[0] = '/'; norm[1] = '\0';
    } else {
        int p = 0;
        for (int i = 0; i < nseg && p < 254; i++) {
            norm[p++] = '/';
            for (int j = 0; j < segs_len[i] && p < 255; j++)
                norm[p++] = abs[segs_start[i] + j];
        }
        norm[p] = '\0';
    }

    // Verify the resolved path exists and is a directory
    vfs_node_t* node = vfs_walk_path(vfs_root, norm);
    if (!node) {
        return -1;
    }
    if (node->type != VFS_DIRECTORY) {
        return -1;
    }

    int i = 0;
    while (norm[i] && i < 255) { current_task->cwd[i] = norm[i]; i++; }
    current_task->cwd[i] = '\0';

    return 0;
}

// getcwd() — get the current working directory path
int sys_getcwd(struct syscall_frame* regs) {
    char*    buf  = (char*)regs->ebx;
    uint32_t size = regs->ecx;

    if (!current_task) return -1;
    if (!buf || size == 0) return -1;
    if (!validate_user_ptr(buf, size)) return -1;

    uint32_t len = 0;
    while (current_task->cwd[len]) len++;
    len++;   // include null terminator

    if (len > size) return -1;

    for (uint32_t i = 0; i < len; i++)
        buf[i] = current_task->cwd[i];

    return (int)buf;
}

// chroot() — change the root directory (root only)
int sys_chroot(char* path) {
    if (!validate_user_str(path)) return -1;
    if (!current_task) return -1;
    if (current_task->euid != 0) return -1;   // root only
    vfs_node_t* node = _resolve_path(path);
    if (!node || node->type != VFS_DIRECTORY) return -1;
    current_task->root = node;
    return 0;
}