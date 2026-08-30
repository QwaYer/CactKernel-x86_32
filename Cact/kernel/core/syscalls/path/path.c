#include "path.h"
#include "validate.h"

static inline file_t *_get_file(int fd) {
    if (!current_task) return 0;
    if (fd < 0 || fd >= MAX_FD) return 0;
    return current_task->proc->fds->files[fd];
}

int sys_create(char *name) {
    if (!current_task) return -1;

    char *kname = copy_path_from_user(name);
    if (!kname) return -1;

    char basename[128];
    vfs_node_t *parent = _resolve_parent(kname, basename, 128);
    kfree(kname);
    if (!parent || !basename[0]) return -1;

    return create_vfs(parent, basename);
}

int sys_mkdir(char *pathname) {
    if (!current_task) return -1;

    char *kpath = copy_path_from_user(pathname);
    if (!kpath) return -1;

    char basename[128];
    vfs_node_t *parent = _resolve_parent(kpath, basename, 128);
    kfree(kpath);
    if (!parent || !basename[0]) return -1;

    if (vfs_check_perm(parent, VFS_PERM_WRITE | VFS_PERM_EXEC) < 0) return -1;

    return mkdir_vfs(parent, basename);
}

int sys_rmdir(char *pathname) {
    if (!validate_user_str(pathname)) return -1;
    if (!current_task) return -1;

    char *kpath = copy_path_from_user(pathname);
    if (!kpath) return -1;

    char basename[128];
    vfs_node_t *parent = _resolve_parent(kpath, basename, 128);
    kfree(kpath);
    if (!parent || !basename[0]) return -1;

    if (vfs_check_perm(parent, VFS_PERM_WRITE | VFS_PERM_EXEC) < 0) return -1;

    return rmdir_vfs(parent, basename);
}

int sys_delete(char *name) {
    if (!current_task) return -1;

    char *kname = copy_path_from_user(name);
    if (!kname) return -1;

    char basename[128];
    vfs_node_t *parent = _resolve_parent(kname, basename, 128);
    kfree(kname);
    if (!parent || !basename[0]) return -1;
    if (vfs_check_perm(parent, VFS_PERM_WRITE | VFS_PERM_EXEC) < 0) return -1;

    return delete_vfs(parent, basename);
}

int sys_unlink(char *path) {
    if (!current_task) return -1;

    char *kpath = copy_path_from_user(path);
    if (!kpath) return -1;

    char basename[128];
    vfs_node_t *parent = _resolve_parent_follow(kpath, basename, 128);
    kfree(kpath);
    if (!parent || !basename[0]) return -1;
    if (vfs_check_perm(parent, VFS_PERM_WRITE | VFS_PERM_EXEC) < 0) return -1;

    return vfs_unlink(parent, basename);
}

int sys_rename(char *oldpath, char *newpath) {
    if (!current_task) return -1;

    char *kold = copy_path_from_user(oldpath);
    if (!kold) return -1;
    char *knew = copy_path_from_user(newpath);
    if (!knew) { kfree(kold); return -1; }

    char old_base[128], new_base[128];
    vfs_node_t *old_parent = _resolve_parent(kold, old_base, 128);
    vfs_node_t *new_parent = _resolve_parent(knew, new_base, 128);
    kfree(kold);
    kfree(knew);

    if (!old_parent || !new_parent) return -1;
    if (!old_base[0] || !new_base[0]) return -1;
    if (old_parent != new_parent) return -1;

    return rename_vfs(old_parent, old_base, new_base);
}

int sys_link(struct syscall_frame *regs) {
    char *oldpath = (char *)regs->ebx;
    char *newpath = (char *)regs->ecx;
    if (!current_task) return -1;

    char *kold = copy_path_from_user(oldpath);
    if (!kold) return -1;
    char *knew = copy_path_from_user(newpath);
    if (!knew) { kfree(kold); return -1; }

    vfs_node_t *target_node = _resolve_path(kold);
    if (!target_node) { kfree(kold); kfree(knew); return -1; }

    char basename[128];
    vfs_node_t *new_parent = _resolve_parent_follow(knew, basename, 128);
    kfree(kold);
    kfree(knew);
    if (!new_parent || !basename[0]) return -1;

    return vfs_link(new_parent, basename, target_node);
}

int sys_symlink(struct syscall_frame *regs) {
    char *target   = (char *)regs->ebx;
    char *linkpath = (char *)regs->ecx;
    if (!current_task) return -1;

    char *ktarget = copy_path_from_user(target);
    if (!ktarget) return -1;
    char *klink   = copy_path_from_user(linkpath);
    if (!klink) { kfree(ktarget); return -1; }

    char basename[128];
    vfs_node_t *parent = _resolve_parent_follow(klink, basename, 128);
    kfree(ktarget);
    kfree(klink);
    if (!parent || !basename[0]) return -1;

    return vfs_symlink(parent, basename, ktarget);
}

int sys_readlink(struct syscall_frame *regs) {
    char     *path  = (char *)regs->ebx;
    char     *buf   = (char *)regs->ecx;
    uint32_t  bufsz = regs->edx;

    if (!validate_user_str(path))       return -1;
    if (!validate_user_ptr(buf, bufsz)) return -1;
    if (bufsz == 0)                     return -1;
    if (!current_task)                  return -1;

    char basename[128];
    vfs_node_t *parent = _resolve_parent_follow(path, basename, 128);
    if (!parent || !basename[0]) return -1;

    vfs_node_t *node = finddir_vfs(parent, basename);
    if (!node) return -1;
    if (node->type != VFS_SYMLINK) return -1;

    return vfs_readlink_node(node, buf, bufsz);
}

int sys_getdents(struct syscall_frame *regs) {
    int      fd    = (int)regs->ebx;
    char    *buf   = (char *)regs->ecx;
    uint32_t count = regs->edx;

    if (!current_task) return -1;

    file_t *f = _get_file(fd);
    if (!f || !f->node) return -1;
    if (f->node->type != VFS_DIRECTORY) return -1;

    uint32_t entry_size = sizeof(struct cact_dirent);
    if (!validate_user_ptr(buf, count)) return -1;
    if (count < entry_size) return -1;

    uint32_t written = 0;
    uint32_t index   = f->offset;

    while (written + entry_size <= count && written + entry_size >= written) {
        struct vfs_dirent *de = readdir_vfs(f->node, index);
        if (!de) break;

        struct cact_dirent local;
        local.d_ino = de->inode;

        int i = 0;
        while (de->name[i] && i < 123) { local.d_name[i] = de->name[i]; i++; }
        local.d_name[i] = '\0';

        if (copy_to_user(buf + written, &local, entry_size) != 0) break;

        written += entry_size;
        index++;
    }

    f->offset = index;
    return (int)written;
}

int sys_chdir(struct syscall_frame *regs) {
    char *path = (char *)regs->ebx;

    if (!current_task) return -1;
    if (!validate_user_str(path)) return -1;

    char abs[256];
    if (path[0] == '/') {
        int i = 0;
        while (path[i] && i < 255) { abs[i] = path[i]; i++; }
        abs[i] = '\0';
    } else {
        int p = 0;
        for (int i = 0; current_task->proc->cwd[i] && p < 254; i++)
            abs[p++] = current_task->proc->cwd[i];
        if (p > 0 && abs[p-1] != '/') abs[p++] = '/';
        for (int i = 0; path[i] && p < 255; i++)
            abs[p++] = path[i];
        abs[p] = '\0';
    }

    int segs_start[64], segs_len[64];
    int nseg = 0;
    const char *s = abs;
    while (*s) {
        while (*s == '/') s++;
        if (!*s) break;
        const char *seg = s;
        int slen = 0;
        while (*s && *s != '/') { s++; slen++; }
        if (slen == 1 && seg[0] == '.') continue;
        if (slen == 2 && seg[0] == '.' && seg[1] == '.') {
            if (nseg > 0) nseg--;
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

    vfs_node_t *node = vfs_walk_path(vfs_root, norm);
    if (!node || node->type != VFS_DIRECTORY) return -1;

    int i = 0;
    while (norm[i] && i < 255) { current_task->proc->cwd[i] = norm[i]; i++; }
    current_task->proc->cwd[i] = '\0';

    return 0;
}

int sys_getcwd(struct syscall_frame *regs) {
    char     *buf  = (char *)regs->ebx;
    uint32_t  size = regs->ecx;

    if (!current_task) return -1;
    if (!buf || size == 0) return -1;
    if (!validate_user_ptr(buf, size)) return -1;

    uint32_t len = 0;
    while (current_task->proc->cwd[len]) len++;
    len++;

    if (len > size) return -1;

    for (uint32_t i = 0; i < len; i++)
        buf[i] = current_task->proc->cwd[i];

    return (int)len;
}

int sys_chroot(char *path) {
    if (!current_task) return -1;
    if (current_task->proc->euid != 0) return -1;

    char *kpath = copy_path_from_user(path);
    if (!kpath) return -1;

    vfs_node_t *node = _resolve_path(kpath);
    kfree(kpath);
    if (!node || node->type != VFS_DIRECTORY) return -1;
    current_task->proc->root = node;
    return 0;
}
