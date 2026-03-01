#include "etcfs.h"
#include "vfs.h"
#include "kernel.h"
#include "memory.h"
#include "libc.h"


static etcfs_entry_t  etc_table[ETCFS_MAX_FILES];

static struct vfs_node etc_dir_node;   
static struct vfs_node* ext4_root = 0; 
static int etcfs_initialized = 0;


static etcfs_entry_t* _find(const char* name) {
    for (int i = 0; i < ETCFS_MAX_FILES; i++)
        if (etc_table[i].used && strncmp(etc_table[i].name, name) == 0)
            return &etc_table[i];
    return 0;
}

static etcfs_entry_t* _alloc(void) {
    for (int i = 0; i < ETCFS_MAX_FILES; i++)
        if (!etc_table[i].used)
            return &etc_table[i];
    return 0;
}


static int _load_from_disk(const char* name, char* buf, uint32_t maxsize) {
    if (!ext4_root) return 0;

    struct vfs_node* etc_dir = finddir_vfs(ext4_root, "etc");
    if (!etc_dir) return 0;

    struct vfs_node* file = finddir_vfs(etc_dir, (char*)name);
    if (!file) return 0;

    uint32_t sz = file->size ? file->size : maxsize;
    if (sz > maxsize) sz = maxsize;
    return read_vfs(file, 0, sz, buf);
}

static int _save_to_disk(const char* name, const char* buf, uint32_t size) {
    if (!ext4_root) return -1;

    struct vfs_node* etc_dir = finddir_vfs(ext4_root, "etc");
    if (!etc_dir) {
        if (mkdir_vfs(ext4_root, "etc") < 0) return -1;
        etc_dir = finddir_vfs(ext4_root, "etc");
        if (!etc_dir) return -1;
    }

    struct vfs_node* file = finddir_vfs(etc_dir, (char*)name);
    if (file) {
        delete_vfs(etc_dir, (char*)name);
    }
    if (create_vfs(etc_dir, (char*)name) < 0) return -1;
    file = finddir_vfs(etc_dir, (char*)name);
    if (!file) return -1;

    if (size == 0) return 0;
    return write_vfs(file, 0, size, (char*)buf);
}


static int _etc_file_read(struct vfs_node* node, unsigned int off,
                           unsigned int size, char* buf)
{
    etcfs_entry_t* e = (etcfs_entry_t*)node->ptr;
    if (!e || !e->data) return 0;
    if (off >= e->size) return 0;
    uint32_t avail = e->size - off;
    if ((uint32_t)size > avail) size = avail;
    memcpy(buf, e->data + off, size);
    return (int)size;
}

static int _etc_file_write(struct vfs_node* node, unsigned int off,
                            unsigned int size, char* buf)
{
    etcfs_entry_t* e = (etcfs_entry_t*)node->ptr;
    if (!e) return -1;

    uint32_t newsize = off + size;
    if (newsize > ETCFS_MAX_SIZE) newsize = ETCFS_MAX_SIZE;

    if (!e->data) {
        e->data = (char*)kmalloc(ETCFS_MAX_SIZE);
        if (!e->data) return -1;
        memset(e->data, 0, ETCFS_MAX_SIZE);
    }

    if (newsize > e->size) e->size = newsize;
    memcpy(e->data + off, buf, size);
    e->dirty = 1;
    node->size = e->size;

    _save_to_disk(e->name, e->data, e->size);
    e->dirty = 0;

    return (int)size;
}


static struct vfs_node* _etc_finddir(struct vfs_node* node, char* name) {
    (void)node;
    etcfs_entry_t* e = _find(name);
    if (!e) return 0;
    return &e->node;
}

static void _etc_listdir(struct vfs_node* node) {
    (void)node;
    int any = 0;
    for (int i = 0; i < ETCFS_MAX_FILES; i++) {
        if (etc_table[i].used) {
            kprint("  ");
            kprint(etc_table[i].name);
            kprint("\n");
            any = 1;
        }
    }
    if (!any) kprint("  (empty)\n");
}

static struct vfs_dirent _etc_dirent;

static struct vfs_dirent* _etc_readdir(struct vfs_node* node, unsigned int index) {
    (void)node;
    unsigned int found = 0;
    for (int i = 0; i < ETCFS_MAX_FILES; i++) {
        if (etc_table[i].used) {
            if (found == index) {
                strncpy(_etc_dirent.name, etc_table[i].name, 128);
                _etc_dirent.inode = (unsigned int)i;
                return &_etc_dirent;
            }
            found++;
        }
    }
    return 0;
}

static int _etc_create(struct vfs_node* node, char* name) {
    (void)node;
    return etcfs_create(name);
}

static int _etc_delete(struct vfs_node* node, char* name) {
    (void)node;
    return etcfs_delete(name);
}


static void _load_all_from_disk(void) {
    if (!ext4_root) return;

    struct vfs_node* etc_dir = finddir_vfs(ext4_root, "etc");
    if (!etc_dir) {
        kprint("[etcfs] /etc not found on disk, will create on first write\n");
        return;
    }

    unsigned int idx = 0;
    struct vfs_dirent* de;
    while ((de = readdir_vfs(etc_dir, idx++)) != 0) {
        if (de->name[0] == '.') continue;

        etcfs_entry_t* slot = _alloc();
        if (!slot) {
            kprint("[etcfs] WARNING: table full, skipping ");
            kprint(de->name);
            kprint("\n");
            break;
        }

        char* buf = (char*)kmalloc(ETCFS_MAX_SIZE);
        if (!buf) continue;
        memset(buf, 0, ETCFS_MAX_SIZE);

        int bytes = _load_from_disk(de->name, buf, ETCFS_MAX_SIZE);

        strncpy(slot->name, de->name, ETCFS_NAME_LEN);
        slot->data  = buf;
        slot->size  = (uint32_t)(bytes > 0 ? bytes : 0);
        slot->dirty = 0;
        slot->used  = 1;

        memset(&slot->node, 0, sizeof(struct vfs_node));
        strncpy(slot->node.name, de->name, 128);
        slot->node.type  = VFS_FILE;
        slot->node.size  = slot->size;
        slot->node.read  = _etc_file_read;
        slot->node.write = _etc_file_write;
        slot->node.ptr   = (struct vfs_node*)slot;

        kprint("[etcfs] loaded: /etc/");
        kprint(de->name);
        kprint("\n");
    }
}

//Public api
void etcfs_init(struct vfs_node* ext4_node) {
    if (etcfs_initialized) return;

    memset(etc_table, 0, sizeof(etc_table));
    ext4_root = ext4_node;

    memset(&etc_dir_node, 0, sizeof(struct vfs_node));
    strncpy(etc_dir_node.name, "etc", 128);
    etc_dir_node.type    = VFS_DIRECTORY;
    etc_dir_node.finddir = _etc_finddir;
    etc_dir_node.listdir = _etc_listdir;
    etc_dir_node.readdir = _etc_readdir;
    etc_dir_node.create  = _etc_create;
    etc_dir_node.delete  = _etc_delete;

    etcfs_initialized = 1;

    _load_all_from_disk();

    kprint("[etcfs] initialized\n");
}

struct vfs_node* etcfs_get_root(void) {
    return &etc_dir_node;
}

int etcfs_read(const char* name, char* buf, uint32_t size) {
    etcfs_entry_t* e = _find(name);
    if (!e || !e->data) return 0;
    uint32_t sz = e->size < size ? e->size : size;
    memcpy(buf, e->data, sz);
    return (int)sz;
}

int etcfs_write(const char* name, const char* buf, uint32_t size) {
    etcfs_entry_t* e = _find(name);
    if (!e) {
        if (etcfs_create(name) < 0) return -1;
        e = _find(name);
        if (!e) return -1;
    }

    if (!e->data) {
        e->data = (char*)kmalloc(ETCFS_MAX_SIZE);
        if (!e->data) return -1;
        memset(e->data, 0, ETCFS_MAX_SIZE);
    }

    uint32_t sz = size < ETCFS_MAX_SIZE ? size : ETCFS_MAX_SIZE;
    memcpy(e->data, buf, sz);
    e->size  = sz;
    e->dirty = 1;
    e->node.size = sz;

    _save_to_disk(name, e->data, e->size);
    e->dirty = 0;
    return (int)sz;
}

void etcfs_flush(void) {
    for (int i = 0; i < ETCFS_MAX_FILES; i++) {
        if (etc_table[i].used && etc_table[i].dirty) {
            _save_to_disk(etc_table[i].name,
                          etc_table[i].data,
                          etc_table[i].size);
            etc_table[i].dirty = 0;
        }
    }
}

int etcfs_create(const char* name) {
    if (_find(name)) return 0; 

    etcfs_entry_t* slot = _alloc();
    if (!slot) return -1;

    slot->data = (char*)kmalloc(ETCFS_MAX_SIZE);
    if (!slot->data) return -1;
    memset(slot->data, 0, ETCFS_MAX_SIZE);

    strncpy(slot->name, name, ETCFS_NAME_LEN);
    slot->size  = 0;
    slot->dirty = 0;
    slot->used  = 1;

    memset(&slot->node, 0, sizeof(struct vfs_node));
    strncpy(slot->node.name, name, 128);
    slot->node.type  = VFS_FILE;
    slot->node.size  = 0;
    slot->node.read  = _etc_file_read;
    slot->node.write = _etc_file_write;
    slot->node.ptr   = (struct vfs_node*)slot;

    _save_to_disk(name, "", 0);
    return 0;
}

int etcfs_delete(const char* name) {
    etcfs_entry_t* e = _find(name);
    if (!e) return -1;

    if (e->data) {
        kfree_heap(e->data);
        e->data = 0;
    }
    memset(e, 0, sizeof(etcfs_entry_t));

    if (ext4_root) {
        struct vfs_node* etc_dir = finddir_vfs(ext4_root, "etc");
        if (etc_dir) delete_vfs(etc_dir, (char*)name);
    }
    return 0;
}