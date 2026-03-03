#include "mntfs.h"
#include "etcfs.h"
#include "devfs.h"
#include "procfs.h"
#include "vfs.h"
#include "ext4.h"
#include "ata.h"
#include "memory.h"
#include "libc.h"
#include "kernel.h"


static mntfs_entry_t   mnt_table[MNTFS_MAX_MOUNTS];
static struct vfs_node mntfs_root_node;
static struct vfs_node mntfs_mnt_node;
static struct vfs_node system_wrapper_node;
static struct vfs_node* system_ext4_node = 0;
static int mntfs_initialized = 0;


typedef struct { const char* name; uint16_t base; uint8_t slave; } ata_dev_t;
static const ata_dev_t ata_devmap[] = {
    { "hda", 0x1F0, 0 },
    { "hdb", 0x1F0, 1 },
    { "hdc", 0x170, 0 },
    { "hdd", 0x170, 1 },
};
#define ATA_DEV_COUNT 4


static mntfs_entry_t* _find(const char* name) {
    for (int i = 0; i < MNTFS_MAX_MOUNTS; i++)
        if (mnt_table[i].used && strncmp(mnt_table[i].name, name) == 0)
            return &mnt_table[i];
    return 0;
}

static mntfs_entry_t* _free_slot(void) {
    for (int i = 0; i < MNTFS_MAX_MOUNTS; i++)
        if (!mnt_table[i].used)
            return &mnt_table[i];
    return 0;
}


static void _fstab_add(const char* devname, const char* mntname) {
    char cur[ETCFS_MAX_SIZE];
    memset(cur, 0, sizeof(cur));
    int len = etcfs_read("fstab", cur, ETCFS_MAX_SIZE - 64);
    if (len < 0) len = 0;

    char needle[128];
    int ni = 0;
    for (int i = 0; devname[i] && ni < 62; i++) needle[ni++] = devname[i];
    needle[ni++] = ' ';
    for (int i = 0; mntname[i] && ni < 126; i++) needle[ni++] = mntname[i];
    needle[ni] = '\0';

    for (int i = 0; i <= len - ni; i++) {
        int m = 1;
        for (int j = 0; j < ni; j++)
            if (cur[i+j] != needle[j]) { m = 0; break; }
        if (m) return;
    }

    int pos = len;
    for (int i = 0; devname[i] && pos < (int)sizeof(cur)-2; i++) cur[pos++] = devname[i];
    cur[pos++] = ' ';
    for (int i = 0; mntname[i] && pos < (int)sizeof(cur)-2; i++) cur[pos++] = mntname[i];
    cur[pos++] = '\n';
    cur[pos]   = '\0';
    etcfs_write("fstab", cur, (uint32_t)pos);
}

static void _fstab_remove(const char* mntname) {
    char cur[ETCFS_MAX_SIZE];
    memset(cur, 0, sizeof(cur));
    int len = etcfs_read("fstab", cur, ETCFS_MAX_SIZE - 1);
    if (len <= 0) return;
    cur[len] = '\0';

    char out[ETCFS_MAX_SIZE];
    int  oi = 0, i = 0;
    int  mlen = (int)strlen(mntname);

    while (i < len) {
        int start = i;
        while (i < len && cur[i] != '\n') i++;
        int end = i;
        if (i < len) i++;

        int ti = start;
        while (ti < end && cur[ti] != ' ') ti++;
        while (ti < end && cur[ti] == ' ') ti++;
        int ts = ti;
        while (ti < end && cur[ti] != ' ' && cur[ti] != '\n') ti++;

        int tlen = ti - ts;
        int match = (tlen == mlen);
        if (match)
            for (int k = 0; k < tlen; k++)
                if (cur[ts+k] != mntname[k]) { match = 0; break; }

        if (!match) {
            for (int k = start; k < end && oi < ETCFS_MAX_SIZE-2; k++)
                out[oi++] = cur[k];
            if (oi < ETCFS_MAX_SIZE-1) out[oi++] = '\n';
        }
    }
    out[oi] = '\0';
    etcfs_write("fstab", out, (uint32_t)oi);
}

static void _fstab_mount_all(void) {
    char buf[ETCFS_MAX_SIZE];
    memset(buf, 0, sizeof(buf));
    int len = etcfs_read("fstab", buf, ETCFS_MAX_SIZE - 1);
    if (len <= 0) { kprint("[mntfs] fstab empty\n"); return; }
    buf[len] = '\0';

    int i = 0;
    while (i < len) {
        char line[128]; int li = 0;
        while (i < len && buf[i] != '\n' && li < 127) line[li++] = buf[i++];
        line[li] = '\0';
        if (i < len) i++;
        if (li == 0 || line[0] == '#') continue;

        char devname[32], mntname[64];
        int di = 0, mi = 0, p = 0;
        while (p < li && line[p] != ' ') devname[di++] = line[p++];
        devname[di] = '\0';
        while (p < li && line[p] == ' ') p++;
        while (p < li && line[p] != ' ' && line[p] != '\n') mntname[mi++] = line[p++];
        mntname[mi] = '\0';

        if (di == 0 || mi == 0) continue;

        char fullname[MNTFS_NAME_LEN];
        int fi = 0;
        const char* pfx = "system/mnt/";
        for (int k = 0; pfx[k] && fi < MNTFS_NAME_LEN-1; k++) fullname[fi++] = pfx[k];
        for (int k = 0; mntname[k] && fi < MNTFS_NAME_LEN-1; k++) fullname[fi++] = mntname[k];
        fullname[fi] = '\0';

        if (_find(fullname)) continue;

        uint16_t base; uint8_t slave;
        if (mntfs_resolve_device(devname, &base, &slave) < 0) {
            kprint("[fstab] unknown device: "); kprint(devname); kprint("\n");
            continue;
        }
        struct vfs_node* node = ext4_mount_disk(base, slave);
        if (!node) {
            kprint("[fstab] failed: "); kprint(devname); kprint("\n");
            continue;
        }
        strncpy(node->name, mntname, 128);
        mntfs_mount(fullname, devname, node, 2);  
        kprint("[fstab] "); kprint(devname);
        kprint(" -> /system/mnt/"); kprint(mntname); kprint("\n");
    }
}


static struct vfs_node* _system_finddir(struct vfs_node* node, char* name) {
    (void)node;
    char full[MNTFS_NAME_LEN];
    int fi = 0;
    const char* pfx = "system/";
    for (int i = 0; pfx[i] && fi < MNTFS_NAME_LEN-1; i++) full[fi++] = pfx[i];
    for (int i = 0; name[i]  && fi < MNTFS_NAME_LEN-1; i++) full[fi++] = name[i];
    full[fi] = '\0';
    mntfs_entry_t* e = _find(full);
    if (e) return e->target;
    if (system_ext4_node && system_ext4_node->finddir)
        return system_ext4_node->finddir(system_ext4_node, name);
    return 0;
}

static int _is_shadowed_by_mount(const char* dirname) {
    char full[MNTFS_NAME_LEN];
    int fi = 0;
    const char* pfx = "system/";
    for (int i = 0; pfx[i] && fi < MNTFS_NAME_LEN-1; i++) full[fi++] = pfx[i];
    for (int i = 0; dirname[i] && fi < MNTFS_NAME_LEN-1; i++) full[fi++] = dirname[i];
    full[fi] = '\0';
    return _find(full) ? 1 : 0;
}

static void _system_listdir(struct vfs_node* node) {
    (void)node;
    const char* pfx = "system/";
    int plen = (int)strlen(pfx);
    for (int i = 0; i < MNTFS_MAX_MOUNTS; i++) {
        if (!mnt_table[i].used) continue;
        int m = 1;
        for (int k = 0; k < plen; k++)
            if (mnt_table[i].name[k] != pfx[k]) { m = 0; break; }
        if (!m) continue;
        const char* rest = mnt_table[i].name + plen;
        int has_slash = 0;
        for (int k = 0; rest[k]; k++)
            if (rest[k] == '/') { has_slash = 1; break; }
        if (has_slash) continue;
        kprint("  "); kprint((char*)rest);
        kprint("/   ["); kprint(mnt_table[i].source); kprint("]\n");
    }
    if (system_ext4_node && system_ext4_node->readdir) {
        unsigned int idx = 0;
        struct vfs_dirent* de;
        while ((de = system_ext4_node->readdir(system_ext4_node, idx++)) != 0) {
            if (_is_shadowed_by_mount(de->name)) continue;
            kprint("  "); kprint(de->name); kprint("\n");
        }
    }
}

static struct vfs_dirent _sys_dirent;

static struct vfs_dirent* _system_readdir(struct vfs_node* node, unsigned int index) {
    (void)node;
    const char* pfx = "system/";
    int plen = (int)strlen(pfx);
    unsigned int found = 0;
    for (int i = 0; i < MNTFS_MAX_MOUNTS; i++) {
        if (!mnt_table[i].used) continue;
        int m = 1;
        for (int k = 0; k < plen; k++)
            if (mnt_table[i].name[k] != pfx[k]) { m = 0; break; }
        if (!m) continue;
        const char* rest = mnt_table[i].name + plen;
        int has_slash = 0;
        for (int k = 0; rest[k]; k++)
            if (rest[k] == '/') { has_slash = 1; break; }
        if (has_slash) continue;
        if (found == index) {
            strncpy(_sys_dirent.name, rest, 128);
            _sys_dirent.inode = (unsigned int)i;
            return &_sys_dirent;
        }
        found++;
    }
    if (system_ext4_node && system_ext4_node->readdir)
        return system_ext4_node->readdir(system_ext4_node, index - found);
    return 0;
}


static struct vfs_node* _root_finddir(struct vfs_node* node, char* name) {
    (void)node;
    mntfs_entry_t* e = _find(name);
    return e ? e->target : 0;
}

static void _root_listdir(struct vfs_node* node) {
    (void)node;
    int any = 0;
    for (int i = 0; i < MNTFS_MAX_MOUNTS; i++) {
        if (!mnt_table[i].used) continue;
        int has_slash = 0;
        for (int k = 0; mnt_table[i].name[k]; k++)
            if (mnt_table[i].name[k] == '/') { has_slash = 1; break; }
        if (has_slash) continue;
        kprint("  /"); kprint(mnt_table[i].name);
        kprint("   ["); kprint(mnt_table[i].source[0] ? mnt_table[i].source : "?");
        kprint("]\n");
        any = 1;
    }
    if (!any) kprint("  (no disks mounted)\n");
}

static struct vfs_dirent _root_dirent;

static struct vfs_dirent* _root_readdir(struct vfs_node* node, unsigned int index) {
    (void)node;
    unsigned int found = 0;
    for (int i = 0; i < MNTFS_MAX_MOUNTS; i++) {
        if (!mnt_table[i].used) continue;
        int has_slash = 0;
        for (int k = 0; mnt_table[i].name[k]; k++)
            if (mnt_table[i].name[k] == '/') { has_slash = 1; break; }
        if (has_slash) continue;
        if (found == index) {
            strncpy(_root_dirent.name, mnt_table[i].name, 128);
            _root_dirent.inode = (unsigned int)i;
            return &_root_dirent;
        }
        found++;
    }
    return 0;
}


static struct vfs_node* _mnt_finddir(struct vfs_node* node, char* name) {
    (void)node;
    char full[MNTFS_NAME_LEN];
    int fi = 0;
    const char* pfx = "system/mnt/";
    for (int i = 0; pfx[i] && fi < MNTFS_NAME_LEN-1; i++) full[fi++] = pfx[i];
    for (int i = 0; name[i]  && fi < MNTFS_NAME_LEN-1; i++) full[fi++] = name[i];
    full[fi] = '\0';
    mntfs_entry_t* e = _find(full);
    return e ? e->target : 0;
}

static void _mnt_listdir(struct vfs_node* node) {
    (void)node;
    const char* pfx = "system/mnt/";
    int plen = (int)strlen(pfx);
    int any = 0;
    for (int i = 0; i < MNTFS_MAX_MOUNTS; i++) {
        if (!mnt_table[i].used) continue;
        int m = 1;
        for (int k = 0; k < plen; k++)
            if (mnt_table[i].name[k] != pfx[k]) { m = 0; break; }
        if (!m) continue;
        kprint("  "); kprint(mnt_table[i].name + plen);
        kprint("  ["); kprint(mnt_table[i].source); kprint("]\n");
        any = 1;
    }
    if (!any) kprint("  (empty — use: mount <device> <n>)\n");
}

static struct vfs_dirent _mnt_dirent;

static struct vfs_dirent* _mnt_readdir(struct vfs_node* node, unsigned int index) {
    (void)node;
    const char* pfx = "system/mnt/";
    int plen = (int)strlen(pfx);
    unsigned int found = 0;
    for (int i = 0; i < MNTFS_MAX_MOUNTS; i++) {
        if (!mnt_table[i].used) continue;
        int m = 1;
        for (int k = 0; k < plen; k++)
            if (mnt_table[i].name[k] != pfx[k]) { m = 0; break; }
        if (!m) continue;
        if (found == index) {
            strncpy(_mnt_dirent.name, mnt_table[i].name + plen, 128);
            _mnt_dirent.inode = (unsigned int)i;
            return &_mnt_dirent;
        }
        found++;
    }
    return 0;
}


//Public api
int mntfs_resolve_device(const char* devname, uint16_t* base_out, uint8_t* slave_out) {
    for (int i = 0; i < ATA_DEV_COUNT; i++)
        if (strncmp(ata_devmap[i].name, devname) == 0) {
            if (base_out)  *base_out  = ata_devmap[i].base;
            if (slave_out) *slave_out = ata_devmap[i].slave;
            return 0;
        }
    return -1;
}

struct vfs_node* mntfs_resolve_path(const char* path) {
    if (!path || !path[0]) return vfs_root;
    struct vfs_node* cur = vfs_root;
    char seg[128];
    int i = 0;
    while (path[i]) {
        int si = 0;
        while (path[i] && path[i] != '/') seg[si++] = path[i++];
        seg[si] = '\0';
        if (path[i] == '/') i++;
        if (si == 0) continue;
        cur = finddir_vfs(cur, seg);
        if (!cur) return 0;
    }
    return cur;
}


static int _system_read(struct vfs_node* node, unsigned int off,
                         unsigned int size, char* buf) {
    (void)node;
    if (!system_ext4_node || !system_ext4_node->read) return -1;
    return system_ext4_node->read(system_ext4_node, off, size, buf);
}

static int _system_write(struct vfs_node* node, unsigned int off,
                          unsigned int size, char* buf) {
    (void)node;
    if (!system_ext4_node || !system_ext4_node->write) return -1;
    return system_ext4_node->write(system_ext4_node, off, size, buf);
}

static int _system_create(struct vfs_node* node, char* name) {
    (void)node;
    if (!system_ext4_node || !system_ext4_node->create) return -1;
    return system_ext4_node->create(system_ext4_node, name);
}

static int _system_delete(struct vfs_node* node, char* name) {
    (void)node;
    if (!system_ext4_node || !system_ext4_node->delete) return -1;
    return system_ext4_node->delete(system_ext4_node, name);
}

static int _system_mkdir(struct vfs_node* node, char* name) {
    (void)node;
    if (!system_ext4_node || !system_ext4_node->mkdir) return -1;
    return system_ext4_node->mkdir(system_ext4_node, name);
}

static int _system_rmdir(struct vfs_node* node, char* name) {
    (void)node;
    if (!system_ext4_node || !system_ext4_node->rmdir) return -1;
    return system_ext4_node->rmdir(system_ext4_node, name);
}

void mntfs_init(void) {
    if (mntfs_initialized) return;
    memset(mnt_table, 0, sizeof(mnt_table));

    memset(&mntfs_root_node, 0, sizeof(struct vfs_node));
    strncpy(mntfs_root_node.name, "/", 128);
    mntfs_root_node.type    = VFS_DIRECTORY;
    mntfs_root_node.finddir = _root_finddir;
    mntfs_root_node.listdir = _root_listdir;
    mntfs_root_node.readdir = _root_readdir;
    vfs_root = &mntfs_root_node;
    mntfs_initialized = 1;

    struct vfs_node* sys = ext4_mount_disk(0x1F0, 0);
    if (!sys) {
        kprint("[mntfs] WARNING: hda has no ext4. Use 'mount hda <n>' manually.\n");
        return;
    }

    system_ext4_node = sys;
    memset(&system_wrapper_node, 0, sizeof(struct vfs_node));
    strncpy(system_wrapper_node.name, "system", 128);
    system_wrapper_node.type    = VFS_DIRECTORY;
    system_wrapper_node.finddir = _system_finddir;
    system_wrapper_node.listdir = _system_listdir;
    system_wrapper_node.readdir = _system_readdir;
    system_wrapper_node.read    = _system_read;
    system_wrapper_node.write   = _system_write;
    system_wrapper_node.create  = _system_create;
    system_wrapper_node.delete  = _system_delete;
    system_wrapper_node.mkdir   = _system_mkdir;
    system_wrapper_node.rmdir   = _system_rmdir;
    mntfs_mount("system", "hda", &system_wrapper_node, 0);

    etcfs_init(sys);
    etcfs_create("fstab");
    mntfs_mount("system/etc", "etcfs", etcfs_get_root(), 0);

    _fstab_mount_all();

    devfs_init();
    mntfs_mount("system/dev", "devfs", devfs_get_root(), 0);

    procfs_init();
    mntfs_mount("system/proc", "procfs", procfs_get_root(), 0);

    memset(&mntfs_mnt_node, 0, sizeof(struct vfs_node));
    strncpy(mntfs_mnt_node.name, "mnt", 128);
    mntfs_mnt_node.type    = VFS_DIRECTORY;
    mntfs_mnt_node.finddir = _mnt_finddir;
    mntfs_mnt_node.listdir = _mnt_listdir;
    mntfs_mnt_node.readdir = _mnt_readdir;
    mntfs_mount("system/mnt", "mntfs", &mntfs_mnt_node, 0);
}

int mntfs_mount(const char* name, const char* source,
                struct vfs_node* target, int persistent) {
    if (!name || !target) return -1;
    if (_find(name)) return -1;
    mntfs_entry_t* slot = _free_slot();
    if (!slot) { kprint("[mntfs] mount table full!\n"); return -1; }

    strncpy(slot->name,   name,              MNTFS_NAME_LEN);
    strncpy(slot->source, source ? source : "", 32);
    slot->target     = target;
    slot->used       = 1;
    slot->persistent = persistent;

    if (persistent == 1 && source && source[0]) {
        const char* pfx = "system/mnt/";
        int plen = (int)strlen(pfx);
        int match = 1;
        for (int k = 0; k < plen; k++)
            if (name[k] != pfx[k]) { match = 0; break; }
        _fstab_add(source, match ? name + plen : name);
    }
    return 0;
}

int mntfs_umount(const char* name) {
    if (!name) return -1;

    const char* protected[] = {
        "system", "system/etc", "system/dev",
        "system/proc", "system/mnt", 0
    };
    for (int i = 0; protected[i]; i++)
        if (strncmp(name, protected[i]) == 0) return -2; 

    mntfs_entry_t* e = _find(name);
    if (!e) return -1;
    if (e->persistent) {
        const char* pfx = "system/mnt/";
        int plen = (int)strlen(pfx);
        int is_mnt = 1;
        for (int k = 0; k < plen; k++)
            if (e->name[k] != pfx[k]) { is_mnt = 0; break; }
        _fstab_remove(is_mnt ? e->name + plen : e->name);
    }
    memset(e, 0, sizeof(mntfs_entry_t));
    return 0;
}

struct vfs_node* mntfs_get(const char* name) {
    if (!name) return 0;
    mntfs_entry_t* e = _find(name);
    return e ? e->target : 0;
}

struct vfs_node* mntfs_get_root(void) { return &mntfs_root_node; }

void mntfs_list(void) {
    kprint("\nMount points:\n");
    int any = 0;
    for (int i = 0; i < MNTFS_MAX_MOUNTS; i++) {
        if (!mnt_table[i].used) continue;
        kprint("  /"); kprint(mnt_table[i].name);
        kprint("  <-  ");
        kprint(mnt_table[i].source[0] ? mnt_table[i].source : "internal");
        kprint(mnt_table[i].persistent ? "  [fstab]\n" : "\n");
        any = 1;
    }
    if (!any) kprint("  (nothing mounted)\n");
}