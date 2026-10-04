#ifndef VFS_INTERNAL_H
#define VFS_INTERNAL_H

#include "vfs.h"
#include "sync.h"

// Mount table limits
#define VFS_MOUNT_MAX 32

// Fixed-size symlink pool.
#define VFS_SYMLINK_POOL_SIZE  256
#define VFS_SYMLINK_TARGET_MAX 512

typedef struct {
    vfs_node_t  node;                          // embedded VFS node
    char        target[VFS_SYMLINK_TARGET_MAX];// symlink target path
    int         in_use;                        // allocation flag
} vfs_symlink_entry_t;

typedef struct {
    vfs_node_t *host;       // mount parent directory
    vfs_node_t *target;     // mounted root node
    char        name[128];  // mount name in parent
} vfs_mount_t;

// The mount table, symlink pool and their mutexes moved to the Rust VFS core
// (`Cact/fs/rust_vfs`, symbols vfs_mount/vfs_umount/vfs_symlink/...).  Only the
// sizes above and the two struct shapes remain part of the C ABI, mirrored in
// `rust_vfs/src/abi.rs`.

#endif
