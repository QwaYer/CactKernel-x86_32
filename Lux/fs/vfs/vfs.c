#include "vfs.h"
#include "sync.h"

struct vfs_node* vfs_root = 0;

static mutex_t vfs_mutex;

void vfs_init(void) {
    mutex_init(&vfs_mutex);
}

int read_vfs(struct vfs_node* node, unsigned int offset,
             unsigned int size, char* buffer)
{
    if (!node || !node->read) return -1;
    mutex_lock(&vfs_mutex);
    int r = node->read(node, offset, size, buffer);
    mutex_unlock(&vfs_mutex);
    return r;
}

int write_vfs(struct vfs_node* node, unsigned int offset,
              unsigned int size, char* buffer)
{
    if (!node || !node->write) return -1;
    mutex_lock(&vfs_mutex);
    int r = node->write(node, offset, size, buffer);
    mutex_unlock(&vfs_mutex);
    return r;
}

void open_vfs(struct vfs_node* node) {
    if (node && node->open) node->open(node);
}

void close_vfs(struct vfs_node* node) {
    if (node && node->close) node->close(node);
}

void listdir_vfs(struct vfs_node* node) {
    if (node && node->type == VFS_DIRECTORY && node->listdir) {
        mutex_lock(&vfs_mutex);
        node->listdir(node);
        mutex_unlock(&vfs_mutex);
    }
}

struct vfs_dirent* readdir_vfs(struct vfs_node* node, unsigned int index) {
    if (!node || node->type != VFS_DIRECTORY || !node->readdir) return 0;
    mutex_lock(&vfs_mutex);
    struct vfs_dirent* d = node->readdir(node, index);
    mutex_unlock(&vfs_mutex);
    return d;
}

struct vfs_node* finddir_vfs(struct vfs_node* node, char* name) {
    if (!node || node->type != VFS_DIRECTORY || !node->finddir) return 0;
    mutex_lock(&vfs_mutex);
    struct vfs_node* n = node->finddir(node, name);
    mutex_unlock(&vfs_mutex);
    return n;
}

int create_vfs(struct vfs_node* node, char* name) {
    if (!node || node->type != VFS_DIRECTORY || !node->create) return -1;
    mutex_lock(&vfs_mutex);
    int r = node->create(node, name);
    mutex_unlock(&vfs_mutex);
    return r;
}

int delete_vfs(struct vfs_node* node, char* name) {
    if (!node || node->type != VFS_DIRECTORY || !node->delete) return -1;
    mutex_lock(&vfs_mutex);
    int r = node->delete(node, name);
    mutex_unlock(&vfs_mutex);
    return r;
}

int mkdir_vfs(struct vfs_node* node, char* name) {
    if (!node || node->type != VFS_DIRECTORY || !node->mkdir) return -1;
    mutex_lock(&vfs_mutex);
    int r = node->mkdir(node, name);
    mutex_unlock(&vfs_mutex);
    return r;
}

int rmdir_vfs(struct vfs_node* node, char* name) {
    if (!node || node->type != VFS_DIRECTORY || !node->rmdir) return -1;
    mutex_lock(&vfs_mutex);
    int r = node->rmdir(node, name);
    mutex_unlock(&vfs_mutex);
    return r;
}