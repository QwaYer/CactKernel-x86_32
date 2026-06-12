#include "resolve.h"
#include "task.h"

// Build an absolute path from a potentially relative path using current_task->proc->cwd.
// The result is always null-terminated and fits within abs_max bytes.
void _make_abs(const char* path, char* abs, int abs_max) {
    int p = 0;
    if (path[0] != '/') {
        // Relative path — prepend current working directory
        for (int i = 0; current_task->proc->cwd[i] && p < abs_max - 2; i++)
            abs[p++] = current_task->proc->cwd[i];
        if (p > 0 && abs[p-1] != '/') abs[p++] = '/';
    }
    // Append the given path
    for (int i = 0; path[i] && p < abs_max - 1; i++)
        abs[p++] = path[i];
    abs[p] = '\0';
}

// Resolve a path to a VFS node, following symlinks.
// Returns NULL if the path does not exist.
vfs_node_t* _resolve_path(const char* path) {
    if (!path || !current_task) return 0;
    char abs[512];
    _make_abs(path, abs, 512);
    return vfs_walk_path_follow(vfs_root, abs, 0);
}

// Resolve the parent directory of a path (following symlinks) and extract
// the basename (the final path component). Returns the parent VFS node.
// If the path has no slashes, the parent is cwd and basename is the whole path.
vfs_node_t* _resolve_parent_follow(const char* path,
                                    char* basename_out, int basename_max) {
    if (!path || !current_task) return 0;

    char abs[512];
    _make_abs(path, abs, 512);

    // Find the last slash
    int last_slash = -1;
    for (int i = 0; abs[i]; i++)
        if (abs[i] == '/') last_slash = i;

    if (last_slash < 0) {
        // No slash — the parent is the current working directory
        int i = 0;
        while (path[i] && i < basename_max - 1) { basename_out[i] = path[i]; i++; }
        basename_out[i] = '\0';
        return vfs_walk_path_follow(vfs_root, current_task->proc->cwd, 0);
    }

    // Copy the basename (everything after the last slash)
    const char* bn = abs + last_slash + 1;
    int i = 0;
    while (bn[i] && i < basename_max - 1) { basename_out[i] = bn[i]; i++; }
    basename_out[i] = '\0';

    // If the slash is at position 0, the parent is the VFS root
    if (last_slash == 0) return vfs_root;

    // Build the parent path (everything before the last slash)
    char parent_path[512];
    for (int j = 0; j < last_slash && j < 511; j++)
        parent_path[j] = abs[j];
    parent_path[last_slash] = '\0';

    return vfs_walk_path_follow(vfs_root, parent_path, 0);
}

// Alias for _resolve_parent_follow — used by syscalls that don't need symlink resolution
vfs_node_t* _resolve_parent(const char* path, char* basename_out, int basename_max) {
    return _resolve_parent_follow(path, basename_out, basename_max);
}