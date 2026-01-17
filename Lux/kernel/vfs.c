#include "vfs.h"

struct vfs_node* vfs_root = 0; 

int read_vfs(struct vfs_node* node, unsigned int offset, unsigned int size, char* buffer) {
    if (node && node->read) {
        return node->read(node, offset, size, buffer);
    }
    return 0;
}

int write_vfs(struct vfs_node* node, unsigned int offset, unsigned int size, char* buffer) {
    if (node && node->write) {
        return node->write(node, offset, size, buffer);
    }
    return 0;
}

void open_vfs(struct vfs_node* node) {
    if (node && node->open) {
        node->open(node);
    }
}

void close_vfs(struct vfs_node* node) {
    if (node && node->close) {
        node->close(node);
    }
}

struct vfs_dirent* readdir_vfs(struct vfs_node* node, unsigned int index) {
    if (node && (node->type & VFS_DIRECTORY) && node->readdir) {
        return node->readdir(node, index);
    }
    return 0;
}

struct vfs_node* finddir_vfs(struct vfs_node* node, char* name) {
    if (node && (node->type & VFS_DIRECTORY) && node->finddir) {
        return node->finddir(node, name);
    }
    return 0;
}