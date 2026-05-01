#ifndef SC_RESOLVE_H
#define SC_RESOLVE_H

#include "kernel.h"
#include "vfs.h"

void       _make_abs(const char* path, char* abs, int abs_max);
vfs_node_t* _resolve_path(const char* path);
vfs_node_t* _resolve_parent_follow(const char* path, char* basename_out, int basename_max);
vfs_node_t* _resolve_parent(const char* path, char* basename_out, int basename_max);

#endif /* SC_RESOLVE_H */
