//! Mirrors of the VFS/devfs structs the DRM node glue hands back to the kernel
//! (`Cact/fs/vfs/vfs.h`, `Cact/fs/vfs/devfs/devfs.h`).
//!
//! The card/render nodes are ordinary `vfs_node_t`s whose ops tables point at
//! Rust functions, so the core needs the real layouts — sizes and offsets are
//! pinned below, exactly as for the DRM structs.

use core::ffi::{c_int, c_void};

/* vfs.h */
pub const VFS_CHARDEVICE: u32 = 0x03;
pub const VFS_POLLIN: u32 = 0x001;
pub const VFS_POLLOUT: u32 = 0x004;
pub const VFS_POLLERR: u32 = 0x008;

/* devfs.h */
pub const DEVFS_F_DIR: u32 = 0x08;

/* ── file_t ─────────────────────────────────────────────────────────────── */

/// `file_t` — the file description between the fd table and a node.  The DRM
/// node stores its per-open client in `priv`.
#[repr(C)]
pub struct File {
    pub node: *mut VfsNode,
    pub offset: u32,
    pub flags: u32,
    pub cloexec: u32,
    pub refcount: u32,
    pub priv_: *mut c_void,
}

const _: () = assert!(core::mem::size_of::<File>() == 24);
const _: () = assert!(core::mem::offset_of!(File, priv_) == 20);

/* ── vfs_node_t ─────────────────────────────────────────────────────────── */

#[repr(C)]
pub struct VfsNode {
    pub name: [u8; 128],
    pub ntype: u32,
    pub size: u32,
    pub inode: u32,
    pub refcount: u32,
    pub mode: u32,
    pub uid: u32,
    pub gid: u32,
    pub ops: *mut VfsOps,
    pub fops: *mut VfsFileOps,
    pub priv_: *mut c_void,
}

const _: () = assert!(core::mem::size_of::<VfsNode>() == 168);
const _: () = assert!(core::mem::offset_of!(VfsNode, ops) == 156);
const _: () = assert!(core::mem::offset_of!(VfsNode, fops) == 160);
const _: () = assert!(core::mem::offset_of!(VfsNode, priv_) == 164);

/* ── vfs_file_ops_t ─────────────────────────────────────────────────────── */

#[repr(C)]
pub struct VfsFileOps {
    pub read: Option<extern "C" fn(*mut VfsNode, *mut c_void, u32, u32, *mut u8) -> c_int>,
    pub write: Option<extern "C" fn(*mut VfsNode, *mut c_void, u32, u32, *mut u8) -> c_int>,
    pub ioctl: Option<extern "C" fn(*mut VfsNode, *mut c_void, u32, *mut c_void) -> c_int>,
    pub poll: Option<extern "C" fn(*mut VfsNode, *mut c_void, u32) -> c_int>,
    pub open: Option<extern "C" fn(*mut VfsNode, *mut File)>,
    pub release: Option<extern "C" fn(*mut VfsNode, *mut File)>,
}

const _: () = assert!(core::mem::size_of::<VfsFileOps>() == 24);

/* ── vfs_ops_t ──────────────────────────────────────────────────────────── */

/// Only `mmap_backing` is ever set on a DRM node; the other 24 slots stay NULL,
/// so they are carried as opaque words.  The slot positions are what matter,
/// and the size assert pins them.
#[repr(C)]
pub struct VfsOps {
    pub before: [*mut c_void; 17],
    pub mmap_backing: Option<extern "C" fn(*mut VfsNode, u32, u32, *mut c_int, *mut u32) -> c_int>,
    pub after: [*mut c_void; 7],
}

const _: () = assert!(core::mem::size_of::<VfsOps>() == 100);
const _: () = assert!(core::mem::offset_of!(VfsOps, mmap_backing) == 68);

/* ── vfs_dirent_t ───────────────────────────────────────────────────────── */

#[repr(C)]
pub struct VfsDirent {
    pub name: [u8; 128],
    pub inode: u32,
}

const _: () = assert!(core::mem::size_of::<VfsDirent>() == 132);
const _: () = assert!(core::mem::offset_of!(VfsDirent, inode) == 128);

/* ── devfs_driver_t ─────────────────────────────────────────────────────── */

/// Mirrors `devfs_driver_t` in Cact/fs/vfs/devfs/devfs.h.  The first five
/// slots (read/write/ctl/status/ioctl) stay NULL for a DRM directory entry;
/// `walk`/`readdir` are ours.  The legacy `ctl`/`status` slots are kept
/// because the C struct keeps them for out-of-tree module compatibility.
#[repr(C)]
pub struct DevfsDriver {
    pub before: [*mut c_void; 5],
    pub walk: Option<extern "C" fn(*mut c_void, *const u8) -> *mut VfsNode>,
    pub readdir: Option<extern "C" fn(*mut c_void, u32) -> *mut VfsDirent>,
}

const _: () = assert!(core::mem::size_of::<DevfsDriver>() == 28);

/* ── devfs.h ────────────────────────────────────────────────────────────── */

extern "C" {
    pub fn register_chrdev(
        name: *const u8,
        flags: u32,
        drv: *mut DevfsDriver,
        drv_priv: *mut c_void,
    ) -> *mut c_void;
}

/* ── small C-string helpers (used by the devfs glue) ────────────────────── */

/// `strlcpy` into a fixed byte array, always NUL-terminated.
pub unsafe fn copy_cstr(dst: *mut u8, cap: usize, src: *const u8) {
    if dst.is_null() || cap == 0 {
        return;
    }
    if src.is_null() {
        *dst = 0;
        return;
    }
    let mut i = 0usize;
    while i + 1 < cap {
        let b = *src.add(i);
        *dst.add(i) = b;
        if b == 0 {
            return;
        }
        i += 1;
    }
    *dst.add(cap - 1) = 0;
}

/// `streq`, comparing `name` against a NUL-terminated buffer.
pub unsafe fn cstr_eq(a: *const u8, b: *const u8) -> bool {
    if a.is_null() || b.is_null() {
        return false;
    }
    let mut i = 0usize;
    loop {
        let x = *a.add(i);
        let y = *b.add(i);
        if x != y {
            return false;
        }
        if x == 0 {
            return true;
        }
        i += 1;
    }
}

/// `<prefix><n>` into a fixed array, always NUL-terminated (the `snprintf`
/// the C devfs glue used).
pub unsafe fn fmt_indexed(dst: *mut u8, cap: usize, prefix: &[u8], n: i32) {
    let mut len = 0usize;
    for &b in prefix {
        if len + 1 >= cap {
            *dst.add(cap - 1) = 0;
            return;
        }
        *dst.add(len) = b;
        len += 1;
    }
    let mut digits = [0u8; 10];
    let mut used = 0usize;
    let mut v = n.unsigned_abs();
    loop {
        digits[used] = b'0' + (v % 10) as u8;
        used += 1;
        v /= 10;
        if v == 0 {
            break;
        }
    }
    while used > 0 {
        used -= 1;
        if len + 1 >= cap {
            break;
        }
        *dst.add(len) = digits[used];
        len += 1;
    }
    *dst.add(len) = 0;
}
