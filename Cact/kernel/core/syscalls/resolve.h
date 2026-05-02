#ifndef SC_RESOLVE_H
#define SC_RESOLVE_H

#include "kernel.h"
#include "vfs.h"

// Build an absolute path from a relative path and current_task->cwd
void         _make_abs(const char* path, char* abs, int abs_max);

// Resolve a path to a VFS node, following symlinks
vfs_node_t*  _resolve_path(const char* path);

// Resolve the parent of a path, following symlinks; extract basename
vfs_node_t*  _resolve_parent_follow(const char* path, char* basename_out, int basename_max);

// Alias for _resolve_parent_follow (for callers that don't care about symlink semantics)
vfs_node_t*  _resolve_parent(const char* path, char* basename_out, int basename_max);

#endif 