#ifndef VFS_H
#define VFS_H

#define VFS_FILE        0x01
#define VFS_DIRECTORY   0x02
#define VFS_CHARDEVICE  0x03
#define VFS_BLOCKDEVICE 0x04
#define VFS_PIPE        0x05

struct vfs_node {
    char name[128];    
    unsigned int type; 
    unsigned int size;  
    unsigned int inode;   

    int (*read)(struct vfs_node* node, unsigned int offset, unsigned int size, char* buffer);
    int (*write)(struct vfs_node* node, unsigned int offset, unsigned int size, char* buffer);
    void (*open)(struct vfs_node* node);
    void (*close)(struct vfs_node* node);

    struct vfs_dirent* (*readdir)(struct vfs_node* node, unsigned int index);
    struct vfs_node* (*finddir)(struct vfs_node* node, char* name);

    struct vfs_node* ptr;  
};

struct vfs_dirent {
    char name[128];
    unsigned int inode;
};

extern struct vfs_node* vfs_root;

int read_vfs(struct vfs_node* node, unsigned int offset, unsigned int size, char* buffer);
int write_vfs(struct vfs_node* node, unsigned int offset, unsigned int size, char* buffer);
void open_vfs(struct vfs_node* node);
void close_vfs(struct vfs_node* node);
struct vfs_dirent* readdir_vfs(struct vfs_node* node, unsigned int index);
struct vfs_node* finddir_vfs(struct vfs_node* node, char* name);

#endif