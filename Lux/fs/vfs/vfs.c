#include "vfs.h"
#include "sync.h"

struct vfs_node* vfs_root = 0;

static mutex_t vfs_mutex;

#define VFS_MOUNT_MAX 16

typedef struct {
    struct vfs_node* host;
    struct vfs_node* mounted;
    char             name[128];
} vfs_mount_t;

static vfs_mount_t mount_table[VFS_MOUNT_MAX];
static int         mount_count = 0;

static int _strcmp(const char* a, const char* b) {
    while (*a && *a == *b) { a++; b++; }
    return *a - *b;
}

static void _strncpy(char* dst, const char* src, int n) {
    int i = 0;
    while (src[i] && i < n - 1) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}

static struct vfs_node* lookup_mount(struct vfs_node* host, char* name) {
    for (int i = 0; i < mount_count; i++)
        if (mount_table[i].host == host && _strcmp(mount_table[i].name, name) == 0)
            return mount_table[i].mounted;
    return 0;
}

void vfs_init(void) {
    mutex_init(&vfs_mutex);
    mount_count = 0;
}

int vfs_mount(struct vfs_node* host, const char* name, struct vfs_node* target) {
    if (!host || !name || !target) return -1;
    if (mount_count >= VFS_MOUNT_MAX) return -1;
    for (int i = 0; i < mount_count; i++)
        if (mount_table[i].host == host && _strcmp(mount_table[i].name, name) == 0)
            return -1;
    mount_table[mount_count].host    = host;
    mount_table[mount_count].mounted = target;
    _strncpy(mount_table[mount_count].name, name, 128);
    mount_count++;
    return 0;
}

int vfs_umount(struct vfs_node* host, const char* name) {
    for (int i = 0; i < mount_count; i++) {
        if (mount_table[i].host == host && _strcmp(mount_table[i].name, name) == 0) {
            mount_table[i] = mount_table[--mount_count];
            return 0;
        }
    }
    return -1;
}

int read_vfs(struct vfs_node* node, unsigned int offset, unsigned int size, char* buffer) {
    if (!node || !node->read) return -1;
    mutex_lock(&vfs_mutex);
    int r = node->read(node, offset, size, buffer);
    mutex_unlock(&vfs_mutex);
    return r;
}

int write_vfs(struct vfs_node* node, unsigned int offset, unsigned int size, char* buffer) {
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
    if (!node || node->type != VFS_DIRECTORY) return;
    mutex_lock(&vfs_mutex);

    if (node->listdir) node->listdir(node);

    for (int i = 0; i < mount_count; i++) {
        if (mount_table[i].host == node) {
            kprint("  ");
            kprint(mount_table[i].name);
            kprint("/\n");
        }
    }

    mutex_unlock(&vfs_mutex);
}

struct vfs_dirent* readdir_vfs(struct vfs_node* node, unsigned int index) {
    if (!node || node->type != VFS_DIRECTORY || !node->readdir) return 0;
    mutex_lock(&vfs_mutex);
    struct vfs_dirent* d = node->readdir(node, index);
    mutex_unlock(&vfs_mutex);
    return d;
}

struct vfs_node* finddir_vfs(struct vfs_node* node, char* name) {
    if (!node || node->type != VFS_DIRECTORY) return 0;
    mutex_lock(&vfs_mutex);
    struct vfs_node* m = lookup_mount(node, name);
    if (m) { mutex_unlock(&vfs_mutex); return m; }
    struct vfs_node* n = node->finddir ? node->finddir(node, name) : 0;
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