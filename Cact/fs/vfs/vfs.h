#ifndef VFS_H
#define VFS_H

#include <stdint.h>

// VFS node types
#define VFS_FILE        0x01
#define VFS_DIRECTORY   0x02
#define VFS_CHARDEVICE  0x03
#define VFS_BLOCKDEVICE 0x04
#define VFS_PIPE        0x05
#define VFS_SOCKET      0x06
#define VFS_SYMLINK     0x07

// Permission check bits (match POSIX r=4, w=2, x=1)
#define VFS_PERM_READ    0x04
#define VFS_PERM_WRITE   0x02
#define VFS_PERM_EXEC    0x01

// Poll event flags (match Linux POLL*)
#define VFS_POLLIN   0x001
#define VFS_POLLOUT  0x004
#define VFS_POLLERR  0x008
#define VFS_POLLHUP  0x010
#define VFS_POLLNVAL 0x020

// Symlink resolution depth limit
#define VFS_SYMLINK_MAX_DEPTH 8

// Error codes for VFS path resolution
#define ELOOP         40
#define ENAMETOOLONG  36

struct vfs_node;
struct vfs_dirent;

// File description — intermediate between fd table and vfs_node.
// dup() shares the same file_t (POSIX semantics).
typedef struct file {
    struct vfs_node *node;
    uint32_t         offset;
    uint32_t         flags;     // open flags (O_RDWR, O_NONBLOCK, etc.)
    uint32_t         cloexec;
    uint32_t         refcount;
} file_t;

// Per-filesystem operations table — each method may be NULL if unsupported
typedef struct vfs_ops {
    int  (*read)   (struct vfs_node *node, uint32_t off, uint32_t size, char *buf);
    int  (*write)  (struct vfs_node *node, uint32_t off, uint32_t size, char *buf);
    void (*open)   (struct vfs_node *node);
    void (*close)  (struct vfs_node *node);

    struct vfs_node*    (*walk)   (struct vfs_node *dir, const char *name);
    struct vfs_dirent*  (*readdir)(struct vfs_node *dir, uint32_t index);
    void                (*listdir)(struct vfs_node *dir);

    int (*create)  (struct vfs_node *dir, const char *name);
    int (*delete)  (struct vfs_node *dir, const char *name);
    int (*mkdir)   (struct vfs_node *dir, const char *name);
    int (*rmdir)   (struct vfs_node *dir, const char *name);
    int (*rename)  (struct vfs_node *dir, const char *oldname, const char *newname);

    int (*symlink) (struct vfs_node *dir, const char *name, const char *target);
    int (*link)    (struct vfs_node *dir, const char *name, struct vfs_node *target);
    int (*unlink)  (struct vfs_node *dir, const char *name);
    int (*readlink)(struct vfs_node *node, char *buf, uint32_t bufsz);

    int (*ioctl)   (struct vfs_node *node, uint32_t cmd, void *arg);

    // New VFS operations — pull logic out of syscall layer
    int (*truncate)(struct vfs_node *node, uint32_t length);
    int (*chmod)   (struct vfs_node *node, uint32_t mode);
    int (*chown)   (struct vfs_node *node, uint32_t uid, uint32_t gid);
    int (*mknod)   (struct vfs_node *dir, const char *name, uint32_t mode, uint32_t dev);
    int (*stat)    (struct vfs_node *node, uint32_t *buf);   // 4-word stat buffer
    int (*poll)    (struct vfs_node *node, uint32_t events);  // returns ready events
    int (*lseek)   (struct vfs_node *node, int offset, int whence, uint32_t *result);
} vfs_ops_t;

// Generic VFS node — embedded by each filesystem
typedef struct vfs_node {
    char          name[128];
    uint32_t      type;
    uint32_t      size;
    uint32_t      inode;
    uint32_t      refcount;   // hard-link / reference count
    uint32_t      mode;       // permission bits (rwxrwxrwx), 0777 default
    uint32_t      uid;        // owner user id
    uint32_t      gid;        // owner group id
    vfs_ops_t    *ops;        // per-type operations
    void         *priv;       // filesystem-private data
} vfs_node_t;

// Directory entry returned by readdir
typedef struct vfs_dirent {
    char     name[128];
    uint32_t inode;
} vfs_dirent_t;

// Global VFS root — set by the first filesystem mount
extern vfs_node_t *vfs_root;

// Initialise VFS layer (clears mount table, initialises mutexes)
void          vfs_init     (void);

// Mount/unmount a filesystem on a host directory
int           vfs_mount    (vfs_node_t *host, const char *name, vfs_node_t *target);
int           vfs_umount   (vfs_node_t *host, const char *name);

// Walk a path WITHOUT following symlinks
vfs_node_t   *vfs_walk_path       (vfs_node_t *start, const char *path);

// Walk a path following symlinks; sets *err_out to ELOOP on depth overflow
vfs_node_t   *vfs_walk_path_follow(vfs_node_t *start, const char *path, int *err_out);

// Path resolution helpers (moved from syscall layer)
void          vfs_make_abs   (const char *path, char *abs, int abs_max);
vfs_node_t   *vfs_resolve_path       (const char *path);
vfs_node_t   *vfs_resolve_parent     (const char *path, char *basename_out, int basename_max);
vfs_node_t   *vfs_resolve_parent_follow(const char *path, char *basename_out, int basename_max);

// Legacy aliases for compatibility
#define _make_abs vfs_make_abs
#define _resolve_path vfs_resolve_path
#define _resolve_parent vfs_resolve_parent
#define _resolve_parent_follow vfs_resolve_parent_follow
#define _fill_stat vfs_fill_stat
#define _vfs_type_to_mode vfs_type_to_mode
#define _kstrcpy vfs_strlcpy

// File descriptor helpers
file_t       *file_alloc     (vfs_node_t *node);
void          file_free      (file_t *f);
file_t       *file_ref       (file_t *f);
int           file_unref     (file_t *f);

// Generic I/O wrappers
int           read_vfs     (vfs_node_t *node, uint32_t off, uint32_t size, char *buf);
int           write_vfs    (vfs_node_t *node, uint32_t off, uint32_t size, char *buf);
void          open_vfs     (vfs_node_t *node);
void          close_vfs    (vfs_node_t *node);
int           ioctl_vfs    (vfs_node_t *node, uint32_t cmd, void *arg);

// Directory operations
vfs_dirent_t *readdir_vfs  (vfs_node_t *dir, uint32_t index);
vfs_node_t   *finddir_vfs  (vfs_node_t *dir, char *name);
void          listdir_vfs  (vfs_node_t *dir);
int           create_vfs   (vfs_node_t *dir, char *name);
int           delete_vfs   (vfs_node_t *dir, char *name);
int           mkdir_vfs    (vfs_node_t *dir, char *name);
int           rmdir_vfs    (vfs_node_t *dir, char *name);
int           rename_vfs   (vfs_node_t *dir, const char *oldname, const char *newname);

// Symlink and hard-link VFS operations
vfs_node_t   *vfs_symlink_alloc  (const char *target, uint32_t target_len);
int           vfs_readlink_node  (vfs_node_t *node, char *buf, uint32_t bufsz);
int           vfs_symlink        (vfs_node_t *dir, const char *name, const char *target);
int           vfs_link           (vfs_node_t *dir, const char *name, vfs_node_t *target_node);
int           vfs_unlink         (vfs_node_t *dir, const char *name);

// Reference counting
void          vfs_node_ref       (vfs_node_t *node);
void          vfs_node_unref     (vfs_node_t *node);

// Permission checking (returns 0 on success, -1 on deny)
int           vfs_check_perm     (vfs_node_t *node, uint32_t perm);

// New VFS wrapper functions (dispatch through vfs_ops_t)
int           truncate_vfs (vfs_node_t *node, uint32_t length);
int           chmod_vfs    (vfs_node_t *node, uint32_t mode);
int           chown_vfs    (vfs_node_t *node, uint32_t uid, uint32_t gid);
int           mknod_vfs    (vfs_node_t *dir, const char *name, uint32_t mode, uint32_t dev);
int           stat_vfs     (vfs_node_t *node, uint32_t *buf);
int           poll_vfs     (vfs_node_t *node, uint32_t events);
int           lseek_vfs    (vfs_node_t *node, int offset, int whence, uint32_t *result);

// Helper: convert VFS node type to POSIX stat mode
uint32_t      vfs_type_to_mode(uint32_t type);

// Helper: fill 4-word stat buffer
void          vfs_fill_stat(vfs_node_t *node, uint32_t *buf);

// Helper: bounded string copy (always null-terminates)
void          vfs_strlcpy(char *dst, const char *src, int max);

#endif