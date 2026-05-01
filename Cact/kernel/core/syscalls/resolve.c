#include "resolve.h"
#include "task.h"

void _make_abs(const char* path, char* abs, int abs_max) {
    int p = 0;
    if (path[0] != '/') {
        for (int i = 0; current_task->cwd[i] && p < abs_max - 2; i++)
            abs[p++] = current_task->cwd[i];
        if (p > 0 && abs[p-1] != '/') abs[p++] = '/';
    }
    for (int i = 0; path[i] && p < abs_max - 1; i++)
        abs[p++] = path[i];
    abs[p] = '\0';
}

vfs_node_t* _resolve_path(const char* path) {
    if (!path || !current_task) return 0;
    char abs[512];
    _make_abs(path, abs, 512);
    return vfs_walk_path_follow(vfs_root, abs, 0);
}

vfs_node_t* _resolve_parent_follow(const char* path,
                                    char* basename_out, int basename_max) {
    if (!path || !current_task) return 0;

    char abs[512];
    _make_abs(path, abs, 512);

    int last_slash = -1;
    for (int i = 0; abs[i]; i++)
        if (abs[i] == '/') last_slash = i;

    if (last_slash < 0) {
        int i = 0;
        while (path[i] && i < basename_max - 1) { basename_out[i] = path[i]; i++; }
        basename_out[i] = '\0';
        return vfs_walk_path_follow(vfs_root, current_task->cwd, 0);
    }

    const char* bn = abs + last_slash + 1;
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

vfs_node_t* _resolve_parent(const char* path, char* basename_out, int basename_max) {
    return _resolve_parent_follow(path, basename_out, basename_max);
}
