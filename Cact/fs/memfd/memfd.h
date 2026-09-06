#ifndef MEMFD_H
#define MEMFD_H

#include <stdint.h>
#include "vfs.h"

// memfd.h — anonymous RAM-backed file objects (memfd_create(2) analog).
//
// The backing storage (page array, size, refcounts, locks) is owned by the
// Rust rust_mm crate.  This header declares the exported FFI surface plus the
// C-side VFS glue that wraps a Rust object in a vfs_node so it can be reached
// through read/write/ftruncate/mmap on a normal file descriptor.

// Rust rust_mm FFI entry points (defined in process/memfd.rs).
int memfd_create(const char *name, uint32_t name_len, int flags);  // -> handle, -1 on error
int memfd_ref(int handle);          // open/fork reference
int memfd_close(int handle);        // last-close release
int memfd_read(int handle, uint32_t off, void *buf, uint32_t size);
int memfd_write(int handle, uint32_t off, const void *buf, uint32_t size);
int memfd_truncate(int handle, uint32_t size);
int memfd_size(int handle);
int memfd_map_inc(int handle);      // MAP_SHARED region added
int memfd_map_dec(int handle);      // MAP_SHARED region removed

// C-side VFS glue (memfd.c).
vfs_node_t *memfd_create_vnode(const char *name, int flags);
int         memfd_node_handle(vfs_node_t *node);  // handle, or -1 if not a memfd
int         memfd_fd_handle(int fd);               // handle, or -1 if not a memfd fd

#endif
