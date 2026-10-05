//! `#[repr(C)]` mirrors of the C VFS structs (`Cact/fs/vfs/vfs.h`).
//!
//! The layout is pinned by compile-time asserts so a field inserted on the C
//! side stops the build here instead of letting the kernel read a struct that
//! drifted — the same guard `rust_net`/`rust_drm` use for their mirrors.

use core::ffi::{c_char, c_void};

// VFS node types (must match vfs.h).
pub const VFS_FILE: u32 = 0x01;
pub const VFS_DIRECTORY: u32 = 0x02;
pub const VFS_CHARDEVICE: u32 = 0x03;
pub const VFS_BLOCKDEVICE: u32 = 0x04;
pub const VFS_PIPE: u32 = 0x05;
pub const VFS_SOCKET: u32 = 0x06;
pub const VFS_SYMLINK: u32 = 0x07;

// Permission bits (POSIX rwx).
pub const VFS_PERM_READ: u32 = 0x04;
pub const VFS_PERM_WRITE: u32 = 0x02;
pub const VFS_PERM_EXEC: u32 = 0x01;

// Path-resolution errors / limits.
pub const VFS_SYMLINK_MAX_DEPTH: i32 = 8;
pub const ELOOP: i32 = 40;
pub const ENAMETOOLONG: i32 = 36;

// Table sizes (must match vfs_internal.h).
pub const VFS_MOUNT_MAX: usize = 32;
pub const VFS_SYMLINK_POOL_SIZE: usize = 256;
pub const VFS_SYMLINK_TARGET_MAX: usize = 512;

// Poll event flags (match vfs.h).
pub const VFS_POLLIN: u32 = 0x001;
pub const VFS_POLLOUT: u32 = 0x004;
pub const VFS_POLLERR: u32 = 0x008;
pub const VFS_POLLHUP: u32 = 0x010;
pub const VFS_POLLNVAL: u32 = 0x020;

/// Directory entry returned by `readdir` (`vfs_dirent_t`, 132 bytes).
#[repr(C)]
pub struct VfsDirent {
    pub name:  [c_char; 128],
    pub inode: u32,
}

const _: () = assert!(core::mem::size_of::<VfsDirent>() == 132);
const _: () = assert!(core::mem::offset_of!(VfsDirent, inode) == 128);

/// Rich stat (`cact_statx_t`): POSIX-style metadata the 4-word `cact_stat_t`
/// cannot carry (uid/gid/nlink/size in bytes/blocks).  Times are reported from
/// the wall clock (the VFS does not track per-inode times yet).
#[repr(C)]
pub struct CactStatx {
    pub ino:     u32,
    pub mode:    u32, // full st_mode (type bits | rwxrwxrwx)
    pub nlink:   u32,
    pub uid:     u32,
    pub gid:     u32,
    pub size:    u32,
    pub blksize: u32,
    pub blocks:  u32, // 512-byte blocks
    pub atime:   u32,
    pub mtime:   u32,
    pub ctime:   u32,
    pub type_:   u32, // VFS node type
}

const _: () = assert!(core::mem::size_of::<CactStatx>() == 48);

/// Filesystem statistics (`cact_statfs_t`, 32 bytes / POSIX `struct statfs`).
#[repr(C)]
pub struct CactStatfs {
    pub f_type:    u32, // filesystem type (hash of the fstype name)
    pub f_bsize:   u32, // optimal transfer block size
    pub f_blocks:  u32, // total data blocks
    pub f_bfree:   u32, // free blocks
    pub f_bavail:  u32, // free blocks available to unprivileged users
    pub f_files:   u32, // total inodes
    pub f_ffree:   u32, // free inodes
    pub f_namelen: u32, // maximum filename length
}

const _: () = assert!(core::mem::size_of::<CactStatfs>() == 32);

/// Per-filesystem operations table (`vfs_ops_t`, 25 slots / 100 bytes).
#[repr(C)]
pub struct VfsOps {
    pub read:           Option<unsafe extern "C" fn(*mut VfsNode, u32, u32, *mut c_char) -> i32>, // 0
    pub write:          Option<unsafe extern "C" fn(*mut VfsNode, u32, u32, *mut c_char) -> i32>, // 1
    pub open:           Option<unsafe extern "C" fn(*mut VfsNode)>, // 2
    pub close:          Option<unsafe extern "C" fn(*mut VfsNode)>, // 3
    pub walk:           Option<unsafe extern "C" fn(*mut VfsNode, *const c_char) -> *mut VfsNode>, // 4
    pub readdir:        Option<unsafe extern "C" fn(*mut VfsNode, u32) -> *mut VfsDirent>, // 5
    pub listdir:        Option<unsafe extern "C" fn(*mut VfsNode)>, // 6
    pub create:         Option<unsafe extern "C" fn(*mut VfsNode, *const c_char) -> i32>, // 7
    pub delete:         Option<unsafe extern "C" fn(*mut VfsNode, *const c_char) -> i32>, // 8
    pub mkdir:          Option<unsafe extern "C" fn(*mut VfsNode, *const c_char) -> i32>, // 9
    pub rmdir:          Option<unsafe extern "C" fn(*mut VfsNode, *const c_char) -> i32>, // 10
    pub rename:         Option<unsafe extern "C" fn(*mut VfsNode, *const c_char, *const c_char) -> i32>, // 11
    pub symlink:        Option<unsafe extern "C" fn(*mut VfsNode, *const c_char, *const c_char) -> i32>, // 12
    pub link:           Option<unsafe extern "C" fn(*mut VfsNode, *const c_char, *mut VfsNode) -> i32>, // 13
    pub unlink:         Option<unsafe extern "C" fn(*mut VfsNode, *const c_char) -> i32>, // 14
    pub readlink:       Option<unsafe extern "C" fn(*mut VfsNode, *mut c_char, u32) -> i32>, // 15
    pub ioctl:          Option<unsafe extern "C" fn(*mut VfsNode, u32, *mut c_void) -> i32>, // 16
    pub mmap_backing:   Option<unsafe extern "C" fn(*mut VfsNode, u32, u32, *mut i32, *mut u32) -> i32>, // 17
    pub truncate:       Option<unsafe extern "C" fn(*mut VfsNode, u32) -> i32>, // 18
    pub chmod:          Option<unsafe extern "C" fn(*mut VfsNode, u32) -> i32>, // 19
    pub chown:          Option<unsafe extern "C" fn(*mut VfsNode, u32, u32) -> i32>, // 20
    pub mknod:          Option<unsafe extern "C" fn(*mut VfsNode, *const c_char, u32, u32) -> i32>, // 21
    pub stat:           Option<unsafe extern "C" fn(*mut VfsNode, *mut u32) -> i32>, // 22
    pub poll:           Option<unsafe extern "C" fn(*mut VfsNode, u32) -> i32>, // 23
    pub lseek:          Option<unsafe extern "C" fn(*mut VfsNode, i32, i32, *mut u32) -> i32>, // 24
    /// Cross-directory rename for directories (appended last; NULL = the FS
    /// cannot move a directory across directories, so the VFS returns -EINVAL).
    pub rename2:        Option<unsafe extern "C" fn(*mut VfsNode, *const c_char, *mut VfsNode, *const c_char) -> i32>, // 25
}

const _: () = assert!(core::mem::size_of::<VfsOps>() == 104);
const _: () = assert!(core::mem::offset_of!(VfsOps, mmap_backing) == 68);
const _: () = assert!(core::mem::offset_of!(VfsOps, poll) == 92);
const _: () = assert!(core::mem::offset_of!(VfsOps, rename2) == 100);

/// Generic VFS node (`vfs_node_t`, 168 bytes).
#[repr(C)]
pub struct VfsNode {
    pub name:     [c_char; 128],
    pub type_:    u32,
    pub size:     u32,
    pub inode:    u32,
    pub refcount: u32,
    pub mode:     u32,
    pub uid:      u32,
    pub gid:      u32,
    pub ops:      *mut VfsOps,
    pub fops:     *mut c_void,
    pub priv_:    *mut c_void,
}

const _: () = assert!(core::mem::size_of::<VfsNode>() == 168);
const _: () = assert!(core::mem::offset_of!(VfsNode, type_) == 128);
const _: () = assert!(core::mem::offset_of!(VfsNode, ops) == 156);
const _: () = assert!(core::mem::offset_of!(VfsNode, fops) == 160);
const _: () = assert!(core::mem::offset_of!(VfsNode, priv_) == 164);

/// Pool entry for a synthetic symlink (`vfs_symlink_entry_t` in vfs_internal.h).
/// The embedded node is first so a `*mut VfsNode` can be recovered as the
/// entry by keeping the pointer unchanged.
#[repr(C)]
pub struct VfsSymlinkEntry {
    pub node:   VfsNode,
    pub target: [c_char; VFS_SYMLINK_TARGET_MAX],
    pub in_use: i32,
}

const _: () = assert!(core::mem::size_of::<VfsSymlinkEntry>() == 684);

/// File description (`file_t`, 24 bytes): the intermediate between the fd table
/// and a node.  `dup()` shares one `file_t`, so `offset`/`flags` are shared.
#[repr(C)]
pub struct File {
    pub node:     *mut VfsNode,
    pub offset:   u32,
    pub flags:    u32,
    pub cloexec:  u32,
    pub refcount: u32,
    pub priv_:    *mut c_void,
}

const _: () = assert!(core::mem::size_of::<File>() == 24);
const _: () = assert!(core::mem::offset_of!(File, priv_) == 20);

/// Per-open node operations (`vfs_file_ops_t`, 24 bytes).  A node with these
/// keeps per-`open()` state in `file_t.priv`, shared by `dup()`d descriptors.
#[repr(C)]
pub struct VfsFileOps {
    pub read:    Option<unsafe extern "C" fn(*mut VfsNode, *mut c_void, u32, u32, *mut c_char) -> i32>,
    pub write:   Option<unsafe extern "C" fn(*mut VfsNode, *mut c_void, u32, u32, *mut c_char) -> i32>,
    pub ioctl:   Option<unsafe extern "C" fn(*mut VfsNode, *mut c_void, u32, *mut c_void) -> i32>,
    pub poll:    Option<unsafe extern "C" fn(*mut VfsNode, *mut c_void, u32) -> i32>,
    pub open:    Option<unsafe extern "C" fn(*mut VfsNode, *mut File)>,
    pub release: Option<unsafe extern "C" fn(*mut VfsNode, *mut File)>,
}

const _: () = assert!(core::mem::size_of::<VfsFileOps>() == 24);

/// Mount-table entry: a filesystem instance mounted at `host/<name>`.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct VfsMount {
    pub host:   *mut VfsNode,
    pub target: *mut VfsNode,
    pub name:   [c_char; 128],
    /// Filesystem type name ("tmpfs", "devfs", …) for /proc/mounts; may be "".
    pub fstype: [c_char; 32],
}
