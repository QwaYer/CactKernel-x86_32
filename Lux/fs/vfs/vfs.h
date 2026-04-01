#ifndef VFS_H
#define VFS_H

#include <stdint.h>


#define VFS_FILE        0x01
#define VFS_DIRECTORY   0x02
#define VFS_CHARDEVICE  0x03
#define VFS_BLOCKDEVICE 0x04
#define VFS_PIPE        0x05
#define VFS_SOCKET      0x06
#define VFS_SYMLINK     0x07

/* Symlink resolution depth limit */
#define VFS_SYMLINK_MAX_DEPTH 8

/* Error code returned when symlink depth is exceeded */
#define ELOOP 40

struct vfs_node;
struct vfs_dirent;


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

    /* Symlink and hard-link operations (may be NULL if not supported) */
    int (*symlink) (struct vfs_node *dir, const char *name, const char *target);
    int (*link)    (struct vfs_node *dir, const char *name, struct vfs_node *target);
    int (*unlink)  (struct vfs_node *dir, const char *name);
    int (*readlink)(struct vfs_node *node, char *buf, uint32_t bufsz);

    int (*ioctl) (struct vfs_node *node, uint32_t cmd, void *arg);
} vfs_ops_t;

typedef struct vfs_node {
    char          name[128];
    uint32_t      type;
    uint32_t      size;
    uint32_t      inode;
    uint32_t      refcount;   /* hard-link / reference count */
    vfs_ops_t    *ops;
    void         *priv;
} vfs_node_t;

typedef struct vfs_dirent {
    char     name[128];
    uint32_t inode;
} vfs_dirent_t;

extern vfs_node_t *vfs_root;


/* Public API */
void          vfs_init     (void);


int           vfs_mount    (vfs_node_t *host, const char *name, vfs_node_t *target);
int           vfs_umount   (vfs_node_t *host, const char *name);


/* Walk a path without following symlinks */
vfs_node_t   *vfs_walk_path       (vfs_node_t *start, const char *path);

/* Walk a path following symlinks; sets *err_out to ELOOP on depth overflow */
vfs_node_t   *vfs_walk_path_follow(vfs_node_t *start, const char *path, int *err_out);


int           read_vfs     (vfs_node_t *node, uint32_t off, uint32_t size, char *buf);
int           write_vfs    (vfs_node_t *node, uint32_t off, uint32_t size, char *buf);
void          open_vfs     (vfs_node_t *node);
void          close_vfs    (vfs_node_t *node);
int           ioctl_vfs    (vfs_node_t *node, uint32_t cmd, void *arg);


vfs_dirent_t *readdir_vfs  (vfs_node_t *dir, uint32_t index);
vfs_node_t   *finddir_vfs  (vfs_node_t *dir, char *name);
void          listdir_vfs  (vfs_node_t *dir);
int           create_vfs   (vfs_node_t *dir, char *name);
int           delete_vfs   (vfs_node_t *dir, char *name);
int           mkdir_vfs    (vfs_node_t *dir, char *name);
int           rmdir_vfs    (vfs_node_t *dir, char *name);
int           rename_vfs   (vfs_node_t *dir, const char *oldname, const char *newname);

/* Symlink and hard-link VFS operations */
vfs_node_t   *vfs_symlink_alloc  (const char *target, uint32_t target_len);
int           vfs_readlink_node  (vfs_node_t *node, char *buf, uint32_t bufsz);
int           vfs_symlink        (vfs_node_t *dir, const char *name, const char *target);
int           vfs_link           (vfs_node_t *dir, const char *name, vfs_node_t *target_node);
int           vfs_unlink         (vfs_node_t *dir, const char *name);

/* Reference counting */
void          vfs_node_ref       (vfs_node_t *node);
void          vfs_node_unref     (vfs_node_t *node);

#endif