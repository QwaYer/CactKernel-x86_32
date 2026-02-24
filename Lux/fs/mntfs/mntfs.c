#include "mntfs.h"
#include "vfs.h"
#include "memory.h"
#include "libc.h"
#include "kernel.h"
#include "ata.h"
#include "ext4.h"

static mntfs_entry_t   mnt_table[MNTFS_MAX_MOUNTS];
static struct vfs_node mntfs_root_node;
static int             mntfs_initialized = 0;


static struct vfs_node disk_roots[4];
static int             disk_roots_used[4];


static mntfs_entry_t* _find_entry(const char* name)
{
    for (int i = 0; i < MNTFS_MAX_MOUNTS; i++)
        if (mnt_table[i].used && strncmp(mnt_table[i].name, name) == 0)
            return &mnt_table[i];
    return 0;
}

static mntfs_entry_t* _free_slot(void)
{
    for (int i = 0; i < MNTFS_MAX_MOUNTS; i++)
        if (!mnt_table[i].used)
            return &mnt_table[i];
    return 0;
}


static struct vfs_node* _mnt_finddir(struct vfs_node* node, char* name)
{
    (void)node;
    mntfs_entry_t* e = _find_entry(name);
    if (!e) return 0;
    return e->target;
}

static void _mnt_listdir(struct vfs_node* node)
{
    (void)node;
    int any = 0;
    for (int i = 0; i < MNTFS_MAX_MOUNTS; i++) {
        if (mnt_table[i].used) {
            kprint("  ");
            kprint(mnt_table[i].name);
            kprint("/\n");
            any = 1;
        }
    }
    if (!any)
        kprint("  (no mounted partitions)\n");
}

static struct vfs_dirent _mnt_dirent;

static struct vfs_dirent* _mnt_readdir(struct vfs_node* node, unsigned int index)
{
    (void)node;
    unsigned int found = 0;
    for (int i = 0; i < MNTFS_MAX_MOUNTS; i++) {
        if (mnt_table[i].used) {
            if (found == index) {
                strncpy(_mnt_dirent.name, mnt_table[i].name, 128);
                _mnt_dirent.inode = (unsigned int)i;
                return &_mnt_dirent;
            }
            found++;
        }
    }
    return 0;
}


/*
 * Disk layout (standard ATA):
 *   index 0 -> primary   master  -> hda  (base 0x1F0, slave=0)
 *   index 1 -> primary   slave   -> hdb  (base 0x1F0, slave=1)
 *   index 2 -> secondary master  -> hdc  (base 0x170, slave=0)
 *   index 3 -> secondary slave   -> hdd  (base 0x170, slave=1)
 */
static const uint16_t _ata_base[]  = { 0x1F0, 0x1F0, 0x170, 0x170 };
static const uint8_t  _ata_slave[] = { 0,     1,     0,     1     };
static const char*    _disk_name[] = { "hda", "hdb", "hdc", "hdd" };


static int _probe_ext4_at(uint16_t base, uint8_t slave, uint32_t lba_start)
{
    port_byte_out(base + 6, (slave ? 0xB0 : 0xA0));
    port_byte_in(base + 7);
    port_byte_in(base + 7);
    port_byte_in(base + 7);
    port_byte_in(base + 7);

    port_byte_out(base + 7, 0xEC);
    uint8_t status = port_byte_in(base + 7);
    if (status == 0x00 || status == 0xFF)
        return 0; 

    uint8_t buf[512];
    ata_read_sector(base, slave, lba_start + 2, buf);

    uint16_t magic = *(uint16_t*)(buf + 56);
    return (magic == EXT4_SUPER_MAGIC);
}


static struct vfs_node* _make_disk_root(int index)
{
    if (disk_roots_used[index])
        return &disk_roots[index];

    if (index == 0 && vfs_root) {
        disk_roots[index]      = *vfs_root;
        disk_roots_used[index] = 1;
        return &disk_roots[index];
    }

    if (vfs_root) {
        disk_roots[index]      = *vfs_root;
        disk_roots_used[index] = 1;
        strncpy(disk_roots[index].name, _disk_name[index], 128);
        return &disk_roots[index];
    }

    return 0;
}


//Public API                                                   
void mntfs_init(void)
{
    if (mntfs_initialized) return;

    memset(mnt_table,       0, sizeof(mnt_table));
    memset(disk_roots,      0, sizeof(disk_roots));
    memset(disk_roots_used, 0, sizeof(disk_roots_used));

    memset(&mntfs_root_node, 0, sizeof(struct vfs_node));
    strncpy(mntfs_root_node.name, "mnt", 128);
    mntfs_root_node.type    = VFS_DIRECTORY;
    mntfs_root_node.finddir = _mnt_finddir;
    mntfs_root_node.listdir = _mnt_listdir;
    mntfs_root_node.readdir = _mnt_readdir;

    if (vfs_root)
        vfs_mount(vfs_root, "mnt", &mntfs_root_node);
    else
        kprint("[mntfs] WARNING: vfs_root is NULL, /mnt not mounted\n");

    mntfs_initialized = 1;

    for (int i = 0; i < 4; i++) {
        if (_probe_ext4_at(_ata_base[i], _ata_slave[i], 0)) {
            struct vfs_node* root = _make_disk_root(i);
            if (root)
                mntfs_mount(_disk_name[i], root);
        }
    }
}

int mntfs_mount(const char* name, struct vfs_node* target)
{
    if (!name || !target) {
        kprint("[mntfs] mount: invalid arguments\n");
        return -1;
    }

    if (_find_entry(name)) {
        kprint("[mntfs] mount: ");
        kprint((char*)name);
        kprint(" already mounted\n");
        return -1;
    }

    mntfs_entry_t* slot = _free_slot();
    if (!slot) {
        kprint("[mntfs] mount: mount table full\n");
        return -1;
    }

    strncpy(slot->name, name, MNTFS_NAME_LEN);
    slot->target = target;
    slot->used   = 1;

    vfs_mount(&mntfs_root_node, name, target);
    return 0;
}

int mntfs_umount(const char* name)
{
    if (!name) return -1;

    mntfs_entry_t* e = _find_entry(name);
    if (!e) {
        kprint("[mntfs] umount: ");
        kprint((char*)name);
        kprint(" not mounted\n");
        return -1;
    }

    vfs_umount(&mntfs_root_node, name);
    memset(e, 0, sizeof(mntfs_entry_t));
    return 0;
}

struct vfs_node* mntfs_get(const char* name)
{
    if (!name) return 0;
    mntfs_entry_t* e = _find_entry(name);
    return e ? e->target : 0;
}

struct vfs_node* mntfs_get_root(void)
{
    return &mntfs_root_node;
}

void mntfs_list(void)
{
    kprint("Mount points under /mnt:\n");
    int any = 0;
    for (int i = 0; i < MNTFS_MAX_MOUNTS; i++) {
        if (mnt_table[i].used) {
            kprint("  /mnt/");
            kprint(mnt_table[i].name);
            kprint(" -> ");
            kprint(mnt_table[i].target ? mnt_table[i].target->name : "???");
            kprint("\n");
            any = 1;
        }
    }
    if (!any)
        kprint("  (empty)\n");
}