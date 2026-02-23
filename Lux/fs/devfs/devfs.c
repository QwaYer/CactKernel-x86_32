#include "devfs.h"
#include "vfs.h"
#include "memory.h"
#include "libc.h"
#include "kernel.h"
#include "pipe.h"

static devfs_entry_t devfs_table[DEVFS_MAX_DEVICES];
static struct vfs_node devfs_root_node;
static int devfs_initialized = 0;

static void _strncpy(char* dst, const char* src, int n) {
    int i = 0;
    while (src[i] && i < n - 1) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}

static int _strcmp(const char* a, const char* b) {
    while (*a && *a == *b) { a++; b++; }
    return *a - *b;
}

static int _null_read(struct vfs_node* node, unsigned int off, unsigned int size, char* buf) {
    (void)node; (void)off; (void)size; (void)buf;
    return 0;
}

static int _null_write(struct vfs_node* node, unsigned int off, unsigned int size, char* buf) {
    (void)node; (void)off; (void)buf;
    return (int)size;
}

static int _zero_read(struct vfs_node* node, unsigned int off, unsigned int size, char* buf) {
    (void)node; (void)off;
    memset(buf, 0, size);
    return (int)size;
}

static uint32_t _rand_state = 0xDEADC0DE;

static uint32_t _lcg_next(void) {
    _rand_state = _rand_state * 1664525u + 1013904223u;
    return _rand_state;
}

static int _random_read(struct vfs_node* node, unsigned int off, unsigned int size, char* buf) {
    (void)node; (void)off;
    unsigned int i = 0;
    while (i < size) {
        uint32_t r = _lcg_next();
        unsigned int chunk = size - i;
        if (chunk > 4) chunk = 4;
        memcpy(buf + i, &r, chunk);
        i += chunk;
    }
    return (int)size;
}

static int _hda_read(struct vfs_node* node, unsigned int off, unsigned int size, char* buf) {
    (void)node;
    uint8_t sector_buf[512];
    unsigned int lba = off / 512;
    unsigned int written = 0;
    while (written < size) {
        ata_read_sector(lba, sector_buf);
        unsigned int chunk = 512;
        if (chunk > size - written) chunk = size - written;
        memcpy(buf + written, sector_buf, chunk);
        written += chunk;
        lba++;
    }
    return (int)written;
}

static int _hda_write(struct vfs_node* node, unsigned int off, unsigned int size, char* buf) {
    (void)node;
    uint8_t sector_buf[512];
    unsigned int lba = off / 512;
    unsigned int written = 0;
    while (written < size) {
        ata_read_sector(lba, sector_buf);
        unsigned int chunk = 512;
        if (chunk > size - written) chunk = size - written;
        memcpy(sector_buf, buf + written, chunk);
        ata_write_sector(lba, sector_buf);
        written += chunk;
        lba++;
    }
    return (int)written;
}

extern volatile char last_char;
extern volatile int  key_event_happened;

static int _tty_read(struct vfs_node* node, unsigned int off, unsigned int size, char* buf) {
    (void)node; (void)off;
    unsigned int i = 0;
    while (i < size) {
        while (!key_event_happened) schedule();
        key_event_happened = 0;
        buf[i++] = last_char;
        if (last_char == '\n') break;
    }
    return (int)i;
}

static int _tty_write(struct vfs_node* node, unsigned int off, unsigned int size, char* buf) {
    (void)node; (void)off;
    char tmp[256];
    unsigned int i = 0;
    while (i < size) {
        unsigned int chunk = size - i;
        if (chunk >= sizeof(tmp)) chunk = sizeof(tmp) - 1;
        memcpy(tmp, buf + i, chunk);
        tmp[chunk] = '\0';
        kprint(tmp);
        i += chunk;
    }
    return (int)size;
}

static struct vfs_node* _devfs_finddir(struct vfs_node* node, char* name) {
    (void)node;
    for (int i = 0; i < DEVFS_MAX_DEVICES; i++)
        if (devfs_table[i].used && _strcmp(devfs_table[i].name, name) == 0)
            return &devfs_table[i].node;
    return 0;
}

static void _devfs_listdir(struct vfs_node* node) {
    (void)node;
    for (int i = 0; i < DEVFS_MAX_DEVICES; i++)
        if (devfs_table[i].used) {
            kprint("  ");
            kprint(devfs_table[i].name);
            kprint("\n");
        }
}

static struct vfs_dirent _devfs_dirent;

static struct vfs_dirent* _devfs_readdir(struct vfs_node* node, unsigned int index) {
    (void)node;
    unsigned int found = 0;
    for (int i = 0; i < DEVFS_MAX_DEVICES; i++) {
        if (devfs_table[i].used) {
            if (found == index) {
                _strncpy(_devfs_dirent.name, devfs_table[i].name, 128);
                _devfs_dirent.inode = (unsigned int)i;
                return &_devfs_dirent;
            }
            found++;
        }
    }
    return 0;
}

static void _fill_node(struct vfs_node* n, const char* name, unsigned int type,
    int (*read)(struct vfs_node*, unsigned int, unsigned int, char*),
    int (*write)(struct vfs_node*, unsigned int, unsigned int, char*))
{
    memset(n, 0, sizeof(struct vfs_node));
    _strncpy(n->name, name, 128);
    n->type  = type;
    n->read  = read;
    n->write = write;
}

struct vfs_node* devfs_get_root(void) {
    return &devfs_root_node;
}

struct vfs_node* devfs_register(const char* name, struct vfs_node* node) {
    if (!name || !node) return 0;
    for (int i = 0; i < DEVFS_MAX_DEVICES; i++)
        if (devfs_table[i].used && _strcmp(devfs_table[i].name, name) == 0)
            return &devfs_table[i].node;
    for (int i = 0; i < DEVFS_MAX_DEVICES; i++) {
        if (!devfs_table[i].used) {
            _strncpy(devfs_table[i].name, name, 128);
            memcpy(&devfs_table[i].node, node, sizeof(struct vfs_node));
            _strncpy(devfs_table[i].node.name, name, 128);
            devfs_table[i].used = 1;
            return &devfs_table[i].node;
        }
    }
    kprint("[devfs] ERROR: device table full\n");
    return 0;
}

int devfs_unregister(const char* name) {
    for (int i = 0; i < DEVFS_MAX_DEVICES; i++) {
        if (devfs_table[i].used && _strcmp(devfs_table[i].name, name) == 0) {
            devfs_table[i].used = 0;
            memset(&devfs_table[i], 0, sizeof(devfs_entry_t));
            return 0;
        }
    }
    return -1;
}

struct vfs_node* devfs_register_fifo(struct vfs_node* node) {
    if (!node) return 0;
    return devfs_register(node->name, node);
}

void devfs_init(void) {
    if (devfs_initialized) return;

    memset(devfs_table, 0, sizeof(devfs_table));

    memset(&devfs_root_node, 0, sizeof(struct vfs_node));
    _strncpy(devfs_root_node.name, "dev", 128);
    devfs_root_node.type    = VFS_DIRECTORY;
    devfs_root_node.finddir = _devfs_finddir;
    devfs_root_node.listdir = _devfs_listdir;
    devfs_root_node.readdir = _devfs_readdir;

    if (vfs_root)
        vfs_mount(vfs_root, "dev", &devfs_root_node);

    struct vfs_node n;
    _fill_node(&n, "null",    VFS_CHARDEVICE,  _null_read,   _null_write);
    devfs_register("null", &n);
    _fill_node(&n, "zero",    VFS_CHARDEVICE,  _zero_read,   _null_write);
    devfs_register("zero", &n);
    _fill_node(&n, "random",  VFS_CHARDEVICE,  _random_read, _null_write);
    devfs_register("random", &n);
    _fill_node(&n, "urandom", VFS_CHARDEVICE,  _random_read, _null_write);
    devfs_register("urandom", &n);
    _fill_node(&n, "hda",     VFS_BLOCKDEVICE, _hda_read,    _hda_write);
    devfs_register("hda", &n);
    _fill_node(&n, "tty",     VFS_CHARDEVICE,  _tty_read,    _tty_write);
    devfs_register("tty", &n);

    devfs_initialized = 1;
}