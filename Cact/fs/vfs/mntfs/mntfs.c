#include "mntfs.h"
#include "devfs.h"
#include "vfsdev.h"
#include "procfs.h"
#include "tmpfs.h"
#include "mqueue.h"
#include "etcfs.h"
#include "binfs.h"
#include "sbinfs.h"
#include "libfs.h"
#include "varfs.h"
#include "usrfs.h"
#include "vfs.h"
#include "fs_mod.h"
#include "memory.h"
#include "klib.h"
#include "kernel.h"
#include "blkdev.h"

static int mntfs_ready = 0;

// Create a directory on the root node if it does not exist yet.
static int _ensure_dir(vfs_node_t *root, const char *name) {
    if (!root || !name || !name[0] || !root->ops) return -1;
    if (root->ops->walk && root->ops->walk(root, name)) return 0;   // exists
    if (!root->ops->mkdir) return -1;
    return root->ops->mkdir(root, name);
}

// Register one layout mount: host/<name> -> target, tagged with its fs type.
static void _mount(vfs_node_t *host, const char *name, vfs_node_t *target,
                   const char *src) {
    if (vfs_mount_ex(host, name, target, src) == 0) {
        pr_info("%s: /%-6s <- %s\n", "mntfs", name, src);
    } else {
        pr_warn("%s: mount of /%s (%s) failed\n", "mntfs", name, src);
    }
}

// Register a usrmerge symlink at the root: /bin -> usr/bin and friends.
static void _symlink(const char *name, const char *target) {
    if (vfs_symlink(vfs_root, name, target) == 0) {
        pr_info("%s: /%-6s -> %s\n", "mntfs", name, target);
    } else {
        pr_warn("%s: symlink /%s -> %s failed\n", "mntfs", name, target);
    }
}

void mntfs_init(void) {
    if (mntfs_ready) return;
    mntfs_ready = 1;

    // 1. Root "/" = boot filesystem when available, otherwise a RAM rootfs.
    vfs_node_t *ext4 = 0;
    blkdev_t   *boot = blkdev_get_boot();
    if (boot)
        ext4 = fs_mod_mount(boot);

    if (ext4) {
        vfs_root = ext4;
        pr_info("%s: root = %s filesystem at /\n", "mntfs", boot->name);
    } else {
        vfs_root = tmpfs_create_root("/");
        if (!vfs_root) {
            pr_warn("%s: RAM rootfs allocation failed — VFS unusable\n",
                    "mntfs");
            return;
        }
        if (boot) {
            pr_warn("%s: filesystem on %s not available, RAM rootfs at /\n",
                    "mntfs", boot->name);
        } else {
            pr_warn("%s: no boot block device — RAM rootfs at /\n", "mntfs");
        }
    }

    // 2. Initialise the subsystem filesystems.  The /etc, /usr and /var
    //    overlays bind to the root filesystem — the boot ext4 when a disk was
    //    mounted, otherwise the RAM tmpfs — so those trees stay writable either
    //    way.  Binding the raw `ext4` node instead (NULL without a disk) left
    //    their create/mkdir ops dead on a RAM boot, so nothing could be written
    //    under /etc, /usr or /var.
    devfs_init();
    vfsdev_init();
    procfs_init();
    tmpfs_init();
    etcfs_init(vfs_root);
    binfs_init(vfs_root);
    sbinfs_init(vfs_root);
    libfs_init(vfs_root);
    varfs_init(vfs_root);
    usrfs_init(vfs_root);

    // Dynamic synthetic filesystems: procfs creates /proc/<pid> as processes
    // come and go, and devfs gains nodes on runtime register_chrdev().  Their
    // subtrees must never be cached, or the dcache would hand back stale
    // bindings (and, for reused pids, wrong ones).
    vfs_set_nocache(devfs_get_root());
    vfs_set_nocache(procfs_get_root());

    // 3. Create the mountpoint directories (and /home, /mnt) on the root fs,
    //    so the tree looks like a real Linux root.  /bin, /sbin and /lib are
    //    still created as directories so readdir("/") lists them; step 4 then
    //    shadows each with a symlink into /usr.
    static const char *dirs[] = {
        "bin", "sbin", "lib", "usr", "etc", "var",
        "tmp", "dev",  "proc",
        "home", "mnt"
    };
    for (unsigned i = 0; i < sizeof(dirs) / sizeof(dirs[0]); i++) {
        if (_ensure_dir(vfs_root, dirs[i]) < 0) {
            pr_warn("%s: cannot create /%s on root fs\n", "mntfs", dirs[i]);
        }
    }

    // 4. Register the layout.  Userland is usrmerge: /usr/<bin,sbin,lib> are
    //    the real mounts and /bin, /sbin, /lib are symlinks pointing into /usr.
    _mount(vfs_root, "usr",   usrfs_get_root(),   "usrfs");
    _mount(usrfs_get_root(), "bin",  binfs_get_root(),  "binfs");
    _mount(usrfs_get_root(), "sbin", sbinfs_get_root(), "sbinfs");
    _mount(usrfs_get_root(), "lib",  libfs_get_root(),  "libfs");
    _mount(vfs_root, "etc",   etcfs_get_root(),   "etcfs");
    _mount(vfs_root, "var",   varfs_get_root(),   "varfs");
    _mount(vfs_root, "tmp",   tmpfs_get_root(),   "tmpfs");
    _mount(vfs_root, "dev",   devfs_get_root(),   "devfs");
    _mount(vfs_root, "proc",  procfs_get_root(),  "procfs");

    // /mnt: the conventional mount point for user-mounted filesystems.  Give
    // it its own tmpfs instance so it is a real, writable mount and appears in
    // the layout like the other points.
    vfs_node_t *mnt_fs = tmpfs_create_root("/mnt");
    if (mnt_fs)
        _mount(vfs_root, "mnt", mnt_fs, "tmpfs");
    else
        pr_warn("%s: /mnt tmpfs allocation failed\n", "mntfs");

    // /dev/shm: the POSIX shared-memory name space (the directory devfs already
    // creates).  shm_open(3) is a naming layer over an ordinary tmpfs mount: a
    // tmpfs file's contents are a memfd page array, so a MAP_SHARED mapping of
    // the object shares real frames with every other opener and the kernel's
    // shared futex (keyed by the word's physical page) works across them.
    vfs_node_t *shm_fs = tmpfs_create_root("/dev/shm");
    if (shm_fs)
        _mount(devfs_get_root(), "shm", shm_fs, "tmpfs");
    else
        pr_warn("%s: /dev/shm tmpfs allocation failed\n", "mntfs");

    // /dev/mqueue: the POSIX message-queue name space.  Unlike /dev/shm this is
    // a filesystem of its own (Cact/fs/mqueue): its files *are* queues, so
    // mq_open(3) is open-or-create, mq_unlink(3) is unlink(), and a queue
    // unlinked while still open stays alive until the last close.
    vfs_node_t *mq_fs = mqueue_create_root("/dev/mqueue");
    if (mq_fs)
        _mount(devfs_get_root(), "mqueue", mq_fs, "posixmq");
    else
        pr_warn("%s: /dev/mqueue queue filesystem allocation failed\n", "mntfs");

    _symlink("bin",  "usr/bin");
    _symlink("sbin", "usr/sbin");
    _symlink("lib",  "usr/lib");
}
