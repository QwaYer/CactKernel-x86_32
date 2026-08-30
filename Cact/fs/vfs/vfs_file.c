#include "vfs.h"
#include "klib.h"
#include "sync.h"
#include "kernel.h"
#include "task.h"
#include "memory.h"

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
