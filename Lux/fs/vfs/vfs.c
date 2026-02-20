#include "vfs.h"

struct vfs_node* vfs_root = 0;

int read_vfs(struct vfs_node* node, unsigned int offset, unsigned int size, char* buffer) {
    if (node && node->read)
        return node->read(node, offset, size, buffer);
    return -1;
}

int write_vfs(struct vfs_node* node, unsigned int offset, unsigned int size, char* buffer) {
    if (node && node->write)
        return node->write(node, offset, size, buffer);
    return -1;
}

void open_vfs(struct vfs_node* node) {
    if (node && node->open)
        node->open(node);
}

void close_vfs(struct vfs_node* node) {
    if (node && node->close)
        node->close(node);
}

void listdir_vfs(struct vfs_node* node) {
    if (node && node->type == VFS_DIRECTORY && node->listdir)
        node->listdir(node);
}

struct vfs_dirent* readdir_vfs(struct vfs_node* node, unsigned int index) {
    if (node && node->type == VFS_DIRECTORY && node->readdir)
        return node->readdir(node, index);
    return 0;
}

struct vfs_node* finddir_vfs(struct vfs_node* node, char* name) {
    if (node && node->type == VFS_DIRECTORY && node->finddir)
        return node->finddir(node, name);
    return 0;
}

int create_vfs(struct vfs_node* node, char* name) {
    if (node && node->type == VFS_DIRECTORY && node->create)
        return node->create(node, name);
    return -1;
}

int delete_vfs(struct vfs_node* node, char* name) {
    if (node && node->type == VFS_DIRECTORY && node->delete)
        return node->delete(node, name);
    return -1;
}

int mkdir_vfs(struct vfs_node* node, char* name) {
    if (node && node->type == VFS_DIRECTORY && node->mkdir)
        return node->mkdir(node, name);
    return -1;
}

int rmdir_vfs(struct vfs_node* node, char* name) {
    if (node && node->type == VFS_DIRECTORY && node->rmdir)
        return node->rmdir(node, name);
    return -1;
}