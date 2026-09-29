#include "mntfs.h"
#include "devfs.h"
#include "vfsdev.h"
#include "procfs.h"
#include "tmpfs.h"
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

// Register one layout mount: host/<name> -> target.
static void _mount(vfs_node_t *host, const char *name, vfs_node_t *target,
                   const char *src) {
    if (vfs_mount(host, name, target) == 0) {
        pr_info("  %-11s : /%-6s <- %s\n", "mntfs", name, src);
    } else {
        pr_warn("  %-11s : mount of /%s (%s) failed\n", "mntfs", name, src);
    }
}

// Register a usrmerge symlink at the root: /bin -> usr/bin and friends.
static void _symlink(const char *name, const char *target) {
    if (vfs_symlink(vfs_root, name, target) == 0) {
        pr_info("  %-11s : /%-6s -> %s\n", "mntfs", name, target);
    } else {
        pr_warn("  %-11s : symlink /%s -> %s failed\n", "mntfs", name, target);
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
        pr_info("  %-11s : root = %s filesystem at /\n", "mntfs", boot->name);
    } else {
        vfs_root = tmpfs_create_root("/");
        if (!vfs_root) {
            pr_warn("  %-11s : RAM rootfs allocation failed — VFS unusable\n",
                    "mntfs");
            return;
        }
        if (boot) {
            pr_warn("  %-11s : filesystem on %s not available, RAM rootfs at /\n",
                    "mntfs", boot->name);
        } else {
            pr_warn("  %-11s : no boot block device — RAM rootfs at /\n", "mntfs");
        }
    }

    // 2. Initialise the subsystem filesystems (bound to the boot ext4 when
    //    present, otherwise they serve the RAM/cctkfs userland).
    devfs_init();
    vfsdev_init();
    procfs_init();
    tmpfs_init();
    etcfs_init(ext4);
    binfs_init(ext4);
    sbinfs_init(ext4);
    libfs_init(ext4);
    varfs_init(ext4);
    usrfs_init(ext4);

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
            pr_warn("  %-11s : cannot create /%s on root fs\n", "mntfs", dirs[i]);
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
    _symlink("bin",  "usr/bin");
    _symlink("sbin", "usr/sbin");
    _symlink("lib",  "usr/lib");

    pr_info("  %-11s : layout ready: /usr/{bin,sbin,lib} mounted, /bin /sbin"
            " /lib symlinked, /dev /etc /home /mnt /proc /tmp /var\n", "mntfs");
}
