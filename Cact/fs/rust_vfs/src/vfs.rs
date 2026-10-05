//! The VFS core, ported from `Cact/fs/vfs/vfs.c` (plus `listdir_vfs` from
//! `vfs_ops.c`).
//!
//! This owns the mount table, the synthetic-symlink pool, node reference
//! counting and path resolution.  It calls back into the C filesystems through
//! their `vfs_ops_t` (`walk`, `listdir`, `symlink`, …), so every existing
//! filesystem — in tree or a loadable `.cctk` module — keeps working unchanged.
//!
//! Behaviour is a 1:1 port of the C code it replaces; the only structural
//! difference is that the dentry cache and inode/superblock layers are added on
//! top in later stages without changing these symbols or struct layouts.

use core::ffi::{c_char, c_void};
use core::mem::MaybeUninit;
use core::ptr;
use core::sync::atomic::{AtomicU32, Ordering};

use cact_sync::mutex_t;

use crate::abi::*;

unsafe extern "C" {
    fn printk(s: *const c_char);
}

/// Maximum directory depth tracked while resolving `..` physically.
const MAX_PATH_DEPTH: usize = 64;

// ── Globals (the C ABI expects these exact symbols) ─────────────────────

/// Global VFS root — set by the first filesystem mount (mntfs.c).
#[no_mangle]
pub static mut vfs_root: *mut VfsNode = ptr::null_mut();

const SYMLINK_ENTRY_INIT: VfsSymlinkEntry = VfsSymlinkEntry {
    node: VfsNode {
        name:     [0; 128],
        type_:    0,
        size:     0,
        inode:    0,
        refcount: 0,
        mode:     0,
        uid:      0,
        gid:      0,
        ops:      ptr::null_mut(),
        fops:     ptr::null_mut(),
        priv_:    ptr::null_mut(),
    },
    target:  [0; VFS_SYMLINK_TARGET_MAX],
    in_use: 0,
};

static mut SYMLINK_POOL: [VfsSymlinkEntry; VFS_SYMLINK_POOL_SIZE] =
    [SYMLINK_ENTRY_INIT; VFS_SYMLINK_POOL_SIZE];

const MOUNT_INIT: VfsMount = VfsMount {
    host:   ptr::null_mut(),
    target: ptr::null_mut(),
    name:   [0; 128],
    fstype: [0; 32],
};

static mut MOUNT_TABLE: [VfsMount; VFS_MOUNT_MAX] = [MOUNT_INIT; VFS_MOUNT_MAX];
static mut MOUNT_COUNT: usize = 0;

// Sleeping mutexes.  `MaybeUninit` because the C-facing `mutex_t` is initialised
// by `vfs_init` (which also runs the scheduler-aware setup), not const.
static mut VFS_MUTEX: MaybeUninit<mutex_t> = MaybeUninit::uninit();
static mut SYMLINK_MUTEX: MaybeUninit<mutex_t> = MaybeUninit::uninit();

#[inline]
unsafe fn vfs_mutex() -> *mut mutex_t {
    // A single static object, initialised once in `vfs_init` and never moved;
    // the pointer is valid and aligned for the object's whole lifetime.
    ptr::addr_of_mut!(VFS_MUTEX) as *mut mutex_t
}

#[inline]
unsafe fn symlink_mutex() -> *mut mutex_t {
    // SAFETY: see `vfs_mutex`.
    ptr::addr_of_mut!(SYMLINK_MUTEX) as *mut mutex_t
}

// ── Small helpers ───────────────────────────────────────────────────────

#[inline]
unsafe fn streq(a: *const c_char, b: *const c_char) -> bool {
    let mut i = 0isize;
    loop {
        // SAFETY: callers pass NUL-terminated C strings; `i` only advances
        // while the bytes match and neither side has hit its terminator.
        let ca = unsafe { *a.offset(i) };
        let cb = unsafe { *b.offset(i) };
        if ca != cb {
            return false;
        }
        if ca == 0 {
            return true;
        }
        i += 1;
    }
}

#[inline]
pub(crate) unsafe fn strlcpy(dst: *mut c_char, src: *const c_char, max: usize) {
    if max == 0 {
        return;
    }
    let mut i = 0usize;
    // SAFETY: `dst` has `max` writable bytes and `src` is NUL-terminated; the
    // loop writes at most `max - 1` bytes and then the terminator.
    unsafe {
        while i + 1 < max && *src.add(i) != 0 {
            *dst.add(i) = *src.add(i);
            i += 1;
        }
        *dst.add(i) = 0;
    }
}

/// Borrow the `refcount` word as an atomic.  `AtomicU32` shares layout with the
/// plain `u32` field, and every access to it goes through this atomic.
#[inline]
unsafe fn refcnt(node: *mut VfsNode) -> &'static AtomicU32 {
    // SAFETY: `node` is non-null and points at a live `VfsNode`; `refcount` is
    // a `u32` at a 4-byte-aligned offset.
    unsafe { &*(ptr::addr_of_mut!((*node).refcount) as *const AtomicU32) }
}

// ── Mount table ─────────────────────────────────────────────────────────

unsafe fn lookup_mount(host: *mut VfsNode, name: *const c_char) -> *mut VfsNode {
    // SAFETY: `MOUNT_COUNT <= VFS_MOUNT_MAX` is maintained by `vfs_mount`/
    // `vfs_umount`, so every index is in bounds.
    unsafe {
        for i in 0..MOUNT_COUNT {
            let m = &*ptr::addr_of!(MOUNT_TABLE[i]);
            if m.host == host && streq(m.name.as_ptr(), name) {
                return m.target;
            }
        }
    }
    ptr::null_mut()
}

/// Resolve one path segment (mount point first, then the dcache, then the
/// filesystem's `walk`).
unsafe fn walk_one(dir: *mut VfsNode, name: *const c_char) -> *mut VfsNode {
    if dir.is_null() || unsafe { (*dir).type_ } != VFS_DIRECTORY {
        return ptr::null_mut();
    }

    // Mount points are resolved through the mount table every time so their
    // per-walk reference semantics are unchanged; they are never cached.
    unsafe { cact_sync::mutex_lock(vfs_mutex()) };
    let m = unsafe { lookup_mount(dir, name) };
    if !m.is_null() {
        vfs_node_ref(m);
    }
    unsafe { cact_sync::mutex_unlock(vfs_mutex()) };
    if !m.is_null() {
        if unsafe { (*m).type_ } == VFS_DIRECTORY {
            unsafe { crate::dcache::set_parent(m, dir) };
        }
        return m;
    }

    // Positive lookups are cached; a miss falls through to the filesystem.
    let cached = unsafe { crate::dcache::lookup(dir, name) };
    if !cached.is_null() {
        return cached;
    }

    // SAFETY: `dir->ops` is either null or a live `vfs_ops_t` owned by the
    // filesystem; `walk` is one of its method slots.
    let ops = unsafe { (*dir).ops };
    if !ops.is_null() {
        if let Some(walk) = unsafe { (*ops).walk } {
            let child = unsafe { walk(dir, name) };
            if !child.is_null() {
                if unsafe { (*child).type_ } == VFS_DIRECTORY {
                    unsafe { crate::dcache::set_parent(child, dir) };
                }
                unsafe { crate::dcache::insert(dir, name, child) };
            }
            return child;
        }
    }
    ptr::null_mut()
}

// SAFETY: every public entry point below takes the node pointers it dereferences
// as arguments from C code that owns them (or from the internal walkers, which
// only ever hand back live nodes).
#[no_mangle]
pub unsafe extern "C" fn vfs_init() {
    unsafe {
        cact_sync::mutex_init(vfs_mutex());
        cact_sync::mutex_init(symlink_mutex());
        MOUNT_COUNT = 0;
        crate::dcache::reset();
        printk(b"  vfs         : mount table + symlinks ready\n\0".as_ptr() as *const c_char);
    }
}

#[no_mangle]
pub unsafe extern "C" fn vfs_mount(
    host: *mut VfsNode,
    name: *const c_char,
    target: *mut VfsNode,
) -> i32 {
    unsafe { vfs_mount_ex(host, name, target, ptr::null()) }
}

/// As [`vfs_mount`], but records the filesystem type name for `/proc/mounts`.
#[no_mangle]
pub unsafe extern "C" fn vfs_mount_ex(
    host: *mut VfsNode,
    name: *const c_char,
    target: *mut VfsNode,
    fstype: *const c_char,
) -> i32 {
    if host.is_null() || name.is_null() || target.is_null() {
        return -1;
    }
    unsafe { cact_sync::mutex_lock(vfs_mutex()) };
    if unsafe { MOUNT_COUNT } >= VFS_MOUNT_MAX {
        unsafe { cact_sync::mutex_unlock(vfs_mutex()) };
        return -1;
    }
    if !unsafe { lookup_mount(host, name) }.is_null() {
        unsafe { cact_sync::mutex_unlock(vfs_mutex()) };
        return -1; // duplicate mount
    }
    // SAFETY: `MOUNT_COUNT < VFS_MOUNT_MAX`, so the slot is in bounds.
    unsafe {
        let slot = &mut *ptr::addr_of_mut!(MOUNT_TABLE[MOUNT_COUNT]);
        slot.host = host;
        slot.target = target;
        strlcpy(slot.name.as_mut_ptr(), name, 128);
        if fstype.is_null() {
            slot.fstype[0] = 0;
        } else {
            strlcpy(slot.fstype.as_mut_ptr(), fstype, 32);
        }
        MOUNT_COUNT += 1;
        cact_sync::mutex_unlock(vfs_mutex());
    }
    0
}

#[no_mangle]
pub unsafe extern "C" fn vfs_umount(host: *mut VfsNode, name: *const c_char) -> i32 {
    unsafe { cact_sync::mutex_lock(vfs_mutex()) };
    // SAFETY: indices stay below `MOUNT_COUNT <= VFS_MOUNT_MAX`.
    unsafe {
        for i in 0..MOUNT_COUNT {
            let m = &*ptr::addr_of!(MOUNT_TABLE[i]);
            if m.host == host && streq(m.name.as_ptr(), name) {
                MOUNT_COUNT -= 1;
                MOUNT_TABLE[i] = MOUNT_TABLE[MOUNT_COUNT];
                cact_sync::mutex_unlock(vfs_mutex());
                return 0;
            }
        }
        cact_sync::mutex_unlock(vfs_mutex());
    }
    -1
}

// ── Path resolution ─────────────────────────────────────────────────────

#[no_mangle]
pub unsafe extern "C" fn vfs_walk_path(start: *mut VfsNode, path: *const c_char) -> *mut VfsNode {
    let fallback = if start.is_null() {
        unsafe { vfs_root }
    } else {
        start
    };
    if path.is_null() {
        return fallback;
    }
    let mut cur = fallback;
    if cur.is_null() {
        return ptr::null_mut();
    }

    // Directory stack so `..` resolves to the physical parent (mounts included)
    // rather than lexically.
    let mut stack = [ptr::null_mut::<VfsNode>(); MAX_PATH_DEPTH];
    let mut depth = 0usize;
    stack[depth] = cur;
    depth += 1;

    // SAFETY: `path` is a NUL-terminated C string; `p` only advances within it.
    unsafe {
        let mut p = path;
        while *p != 0 && !cur.is_null() {
            while *p == b'/' as c_char {
                p = p.add(1);
            }
            if *p == 0 {
                break;
            }
            let mut seg = [0 as c_char; 128];
            let mut si = 0usize;
            while *p != 0 && *p != b'/' as c_char && si < 127 {
                seg[si] = *p;
                si += 1;
                p = p.add(1);
            }
            if *p != 0 && *p != b'/' as c_char {
                return ptr::null_mut();
            }
            seg[si] = 0;
            if si == 1 && seg[0] == b'.' as c_char {
                continue;
            }
            if si == 2 && seg[0] == b'.' as c_char && seg[1] == b'.' as c_char {
                if depth > 1 {
                    depth -= 1;
                }
                cur = stack[depth - 1];
                continue;
            }
            let node = walk_one(cur, seg.as_ptr());
            if node.is_null() {
                return ptr::null_mut();
            }
            if (*node).type_ == VFS_DIRECTORY && depth < MAX_PATH_DEPTH {
                stack[depth] = node;
                depth += 1;
            }
            cur = node;
        }
    }
    cur
}

#[no_mangle]
pub unsafe extern "C" fn finddir_vfs(dir: *mut VfsNode, name: *const c_char) -> *mut VfsNode {
    if name.is_null() {
        return ptr::null_mut();
    }
    // "." and ".." have no filesystem entry; resolve them here so the final
    // component of a dirfd-relative lookup (e.g. stat("/usr/bin/..")) works.
    unsafe {
        if *name == b'.' as c_char && *name.add(1) == 0 {
            return dir;
        }
        if *name == b'.' as c_char && *name.add(1) == b'.' as c_char && *name.add(2) == 0 {
            let p = crate::dcache::parent_of(dir);
            return if p.is_null() { dir } else { p };
        }
        walk_one(dir, name)
    }
}

/// Resolve a single dirfd-relative name; when `follow` is non-zero and the
/// result is a symlink, follow it (relative targets resolve from `dir`).
#[no_mangle]
pub unsafe extern "C" fn vfs_lookup_child(
    dir: *mut VfsNode,
    name: *const c_char,
    follow: i32,
) -> *mut VfsNode {
    let node = unsafe { finddir_vfs(dir, name) };
    if node.is_null() || follow == 0 {
        return node;
    }
    if unsafe { (*node).type_ } != VFS_SYMLINK {
        return node;
    }
    // SAFETY: `node` is a live symlink; `target` is a bounded local buffer.
    unsafe {
        let mut target = [0 as c_char; VFS_SYMLINK_TARGET_MAX];
        let len = vfs_readlink_node(node, target.as_mut_ptr(), VFS_SYMLINK_TARGET_MAX as u32);
        if len <= 0 {
            return ptr::null_mut();
        }
        let base = if target[0] == b'/' as c_char { vfs_root } else { dir };
        let mut err = 0i32;
        walk_path_follow(base, target.as_ptr(), &mut err)
    }
}

// ── Symlink pool ────────────────────────────────────────────────────────

#[no_mangle]
pub unsafe extern "C" fn vfs_symlink_alloc(
    target: *const c_char,
    target_len: u32,
) -> *mut VfsNode {
    if target.is_null() {
        return ptr::null_mut();
    }
    unsafe { cact_sync::mutex_lock(symlink_mutex()) };
    // SAFETY: `SYMLINK_POOL` is a static array; `i` is in bounds.
    unsafe {
        for i in 0..VFS_SYMLINK_POOL_SIZE {
            let e = &mut *ptr::addr_of_mut!(SYMLINK_POOL[i]);
            if e.in_use != 0 {
                continue;
            }
            e.in_use = 1;
            e.node.type_ = VFS_SYMLINK;
            e.node.refcount = 1;
            e.node.ops = ptr::null_mut();
            e.node.inode = 0;
            e.node.mode = 0o777; // lrwxrwxrwx, like Linux
            e.node.name[0] = 0;

            let copy_len = core::cmp::min(target_len as usize, VFS_SYMLINK_TARGET_MAX - 1);
            let mut j = 0usize;
            while j < copy_len {
                e.target[j] = *target.add(j);
                j += 1;
            }
            e.target[j] = 0;
            e.node.size = copy_len as u32;
            e.node.priv_ = e.target.as_mut_ptr() as *mut c_void;

            cact_sync::mutex_unlock(symlink_mutex());
            return ptr::addr_of_mut!(e.node);
        }
        cact_sync::mutex_unlock(symlink_mutex());
    }
    ptr::null_mut()
}

#[no_mangle]
pub unsafe extern "C" fn vfs_readlink_node(
    node: *mut VfsNode,
    buf: *mut c_char,
    bufsz: u32,
) -> i32 {
    if node.is_null() || unsafe { (*node).type_ } != VFS_SYMLINK || buf.is_null() || bufsz == 0 {
        return -1;
    }
    // SAFETY: `node->ops` is null or a live ops table; if it implements
    // `readlink`, hand the request to the filesystem.
    let ops = unsafe { (*node).ops };
    if !ops.is_null() {
        if let Some(readlink) = unsafe { (*ops).readlink } {
            return unsafe { readlink(node, buf, bufsz) };
        }
    }
    // SAFETY: synthetic symlinks store the target pointer in `node->priv`;
    // `bufsz` bounds how much of that NUL-terminated string is copied.
    unsafe {
        let target = (*node).priv_ as *const c_char;
        if target.is_null() {
            return -1;
        }
        let mut len = 0u32;
        while len + 1 < bufsz && *target.add(len as usize) != 0 {
            *buf.add(len as usize) = *target.add(len as usize);
            len += 1;
        }
        *buf.add(len as usize) = 0;
        len as i32
    }
}

/// Follow-symlink path walk with a directory stack, so `..` resolves to the
/// physical parent (crossing mounts and following symlinks) exactly as Linux
/// does.  Symlink targets are re-injected into the path and re-walked; the
/// link counter bounds nesting.
unsafe fn walk_path_follow(
    start: *mut VfsNode,
    path: *const c_char,
    err: *mut i32,
) -> *mut VfsNode {
    let base = if start.is_null() { unsafe { vfs_root } } else { start };
    if path.is_null() {
        return base;
    }
    if base.is_null() {
        return ptr::null_mut();
    }

    let mut stack = [ptr::null_mut::<VfsNode>(); MAX_PATH_DEPTH];
    let mut depth = 0usize;
    stack[depth] = base;
    depth += 1;
    let mut cur = base;
    let mut links = 0i32;

    // Reroute buffers for symlink expansion.  A two-entry ring is enough: each
    // reroute fills the buffer that the current `p` does *not* point into.
    let mut bufs = [[0 as c_char; 544]; 2];
    let mut buf_sel = 0usize;

    // SAFETY: `path` is NUL-terminated; every buffer write is bounds-checked.
    unsafe {
        let mut p = path;
        while *p != 0 && !cur.is_null() {
            while *p == b'/' as c_char {
                p = p.add(1);
            }
            if *p == 0 {
                break;
            }

            let mut seg = [0 as c_char; 128];
            let mut si = 0usize;
            while *p != 0 && *p != b'/' as c_char && si < 127 {
                seg[si] = *p;
                si += 1;
                p = p.add(1);
            }
            if *p != 0 && *p != b'/' as c_char {
                *err = ENAMETOOLONG;
                return ptr::null_mut();
            }
            seg[si] = 0;

            if si == 1 && seg[0] == b'.' as c_char {
                continue;
            }
            if si == 2 && seg[0] == b'.' as c_char && seg[1] == b'.' as c_char {
                if depth > 1 {
                    depth -= 1;
                }
                cur = stack[depth - 1];
                continue;
            }

            let node = walk_one(cur, seg.as_ptr());
            if node.is_null() {
                return ptr::null_mut();
            }

            if (*node).type_ == VFS_SYMLINK {
                if links >= VFS_SYMLINK_MAX_DEPTH {
                    *err = ELOOP;
                    return ptr::null_mut();
                }
                links += 1;

                let mut target = [0 as c_char; VFS_SYMLINK_TARGET_MAX];
                let len =
                    vfs_readlink_node(node, target.as_mut_ptr(), VFS_SYMLINK_TARGET_MAX as u32);
                if len <= 0 {
                    return ptr::null_mut();
                }

                let idx = buf_sel;
                buf_sel ^= 1;
                let dst = bufs[idx].as_mut_ptr();
                let mut w = 0usize;
                let mut t = 0usize;
                while target[t] != 0 && w + 1 < 544 {
                    *dst.add(w) = target[t];
                    w += 1;
                    t += 1;
                }
                if w + 1 < 544 && (w == 0 || *dst.add(w - 1) != b'/' as c_char) {
                    *dst.add(w) = b'/' as c_char;
                    w += 1;
                }
                let mut q = 0usize;
                while *p.add(q) != 0 && w + 1 < 544 {
                    *dst.add(w) = *p.add(q);
                    w += 1;
                    q += 1;
                }
                *dst.add(w) = 0;

                if *dst == b'/' as c_char {
                    // Absolute target: restart from the root.
                    cur = vfs_root;
                    stack[0] = vfs_root;
                    depth = 1;
                }
                // A relative target resolves from the directory that holds the
                // link, which is the current `cur` (unchanged).
                p = bufs[idx].as_ptr();
                continue;
            }

            if (*node).type_ == VFS_DIRECTORY && depth < MAX_PATH_DEPTH {
                stack[depth] = node;
                depth += 1;
            }
            cur = node;
        }
    }

    if unsafe { *err } != 0 {
        return ptr::null_mut();
    }
    cur
}

#[no_mangle]
pub unsafe extern "C" fn vfs_walk_path_follow(
    start: *mut VfsNode,
    path: *const c_char,
    err_out: *mut i32,
) -> *mut VfsNode {
    let mut err = 0i32;
    let result = unsafe { walk_path_follow(start, path, &mut err) };
    if !err_out.is_null() {
        unsafe { *err_out = err };
    }
    result
}

// ── Node lifetime ───────────────────────────────────────────────────────

#[no_mangle]
pub unsafe extern "C" fn vfs_node_ref(node: *mut VfsNode) {
    if !node.is_null() {
        unsafe { refcnt(node) }.fetch_add(1, Ordering::AcqRel);
    }
}

#[no_mangle]
pub unsafe extern "C" fn vfs_node_unref(node: *mut VfsNode) {
    if node.is_null() {
        return;
    }
    // SAFETY: `node` is a live `VfsNode` supplied by a caller holding a
    // reference; the atomic keeps the decrement race-free.
    let cell = unsafe { refcnt(node) };
    let old = cell.fetch_sub(1, Ordering::AcqRel);
    if old == 0 {
        cell.fetch_add(1, Ordering::AcqRel);
        return;
    }
    if old == 1 && unsafe { (*node).type_ } == VFS_SYMLINK {
        unsafe { cact_sync::mutex_lock(symlink_mutex()) };
        // SAFETY: a symlink node is the first field of its pool entry, so the
        // node pointer *is* the entry pointer; range-check before writing.
        unsafe {
            let base = ptr::addr_of_mut!(SYMLINK_POOL) as *mut VfsSymlinkEntry;
            let end = base.add(VFS_SYMLINK_POOL_SIZE);
            let entry = node as *mut VfsSymlinkEntry;
            if entry >= base && entry < end {
                (*entry).in_use = 0;
            }
            cact_sync::mutex_unlock(symlink_mutex());
        }
    }
}

// ── Links and unlink ────────────────────────────────────────────────────

#[no_mangle]
pub unsafe extern "C" fn vfs_symlink(
    dir: *mut VfsNode,
    name: *const c_char,
    target: *const c_char,
) -> i32 {
    if dir.is_null() || unsafe { (*dir).type_ } != VFS_DIRECTORY || name.is_null() || target.is_null()
    {
        return -1;
    }
    // SAFETY: `dir->ops` is null or a live ops table.
    let ops = unsafe { (*dir).ops };
    if !ops.is_null() {
        if let Some(symlink) = unsafe { (*ops).symlink } {
            return unsafe { symlink(dir, name, target) };
        }
    }

    // SAFETY: `target` is NUL-terminated; measure it before copying.
    let tlen = unsafe {
        let mut n = 0usize;
        while *target.add(n) != 0 {
            n += 1;
        }
        n
    };

    let sym = unsafe { vfs_symlink_alloc(target, tlen as u32) };
    if sym.is_null() {
        return -1;
    }
    // SAFETY: `sym` is a freshly allocated pool node with a writable name field.
    unsafe { strlcpy((*sym).name.as_mut_ptr(), name, 128) };

    let ret = unsafe { vfs_mount(dir, name, sym) };
    if ret != 0 {
        unsafe { vfs_node_unref(sym) };
    }
    ret
}

#[no_mangle]
pub unsafe extern "C" fn vfs_link(
    dir: *mut VfsNode,
    name: *const c_char,
    target_node: *mut VfsNode,
) -> i32 {
    if dir.is_null()
        || unsafe { (*dir).type_ } != VFS_DIRECTORY
        || name.is_null()
        || target_node.is_null()
    {
        return -1;
    }
    // SAFETY: `dir->ops` is null or a live ops table.
    let ops = unsafe { (*dir).ops };
    if ops.is_null() {
        return -1;
    }
    // The filesystem owns inode lifetime: a hard link is one more directory
    // entry pointing at the same inode, and the filesystem tracks the count.
    // The VFS does not touch `vnode.refcount` here (it would double-count).
    match unsafe { (*ops).link } {
        Some(link) => {
            let ret = unsafe { link(dir, name, target_node) };
            if ret == 0 {
                unsafe { crate::dcache::invalidate(dir, name) };
            }
            ret
        }
        None => -1,
    }
}

/// Cross-directory rename.  Same directory delegates to the filesystem's
/// `rename`; across directories a regular file is moved by linking it under the
/// new name and unlinking the old one (atomic: this kernel has no scheduling
/// point between the two steps), and a directory is moved through the
/// filesystem's `rename2` op when it provides one (else -EINVAL).  Existing
/// targets are refused with -EEXIST.
#[no_mangle]
pub unsafe extern "C" fn vfs_rename_at(
    olddir: *mut VfsNode,
    oldname: *const c_char,
    newdir: *mut VfsNode,
    newname: *const c_char,
) -> i32 {
    const EEXIST: i32 = 17;
    const EINVAL: i32 = 22;

    if olddir.is_null() || newdir.is_null() || oldname.is_null() || newname.is_null() {
        return -EINVAL;
    }
    if olddir == newdir {
        return unsafe { crate::ops::rename_vfs(olddir, oldname, newname) };
    }

    let node = unsafe { finddir_vfs(olddir, oldname) };
    if node.is_null() {
        return -2; // ENOENT
    }
    if !unsafe { finddir_vfs(newdir, newname) }.is_null() {
        return -EEXIST;
    }
    // Prefer the filesystem's two-directory rename for any type: it is the
    // only way to move a directory (no link+unlink for dirs), and filesystems
    // like ext4 have no link/unlink op at all.
    let ops = unsafe { (*olddir).ops };
    if !ops.is_null() {
        if let Some(f) = unsafe { (*ops).rename2 } {
            let ret = unsafe { f(olddir, oldname, newdir, newname) };
            if ret == 0 {
                unsafe { crate::dcache::invalidate(olddir, oldname) };
                unsafe { crate::dcache::invalidate(newdir, newname) };
            }
            return ret;
        }
    }
    if unsafe { (*node).type_ } == VFS_DIRECTORY {
        return -EINVAL; // no subtree move without a rename2 op
    }

    let lret = unsafe { vfs_link(newdir, newname, node) };
    if lret != 0 {
        return lret;
    }
    let uret = unsafe { vfs_unlink(olddir, oldname) };
    if uret != 0 {
        // Roll back so a failed move leaves the tree unchanged.
        unsafe { vfs_unlink(newdir, newname) };
        return uret;
    }
    0
}

#[no_mangle]
pub unsafe extern "C" fn vfs_unlink(dir: *mut VfsNode, name: *const c_char) -> i32 {
    if dir.is_null() || unsafe { (*dir).type_ } != VFS_DIRECTORY || name.is_null() {
        return -1;
    }

    // A name shadowed by a mount is removed by unmounting.
    unsafe { cact_sync::mutex_lock(vfs_mutex()) };
    let mut mounted = ptr::null_mut();
    // SAFETY: indices stay below `MOUNT_COUNT <= VFS_MOUNT_MAX`.
    unsafe {
        for i in 0..MOUNT_COUNT {
            let m = &*ptr::addr_of!(MOUNT_TABLE[i]);
            if m.host == dir && streq(m.name.as_ptr(), name) {
                mounted = m.target;
                if !mounted.is_null() {
                    vfs_node_ref(mounted);
                }
                break;
            }
        }
        cact_sync::mutex_unlock(vfs_mutex());
    }

    if !mounted.is_null() && unsafe { (*mounted).type_ } == VFS_SYMLINK {
        let ret = unsafe { vfs_umount(dir, name) };
        unsafe { vfs_node_unref(mounted) };
        return ret;
    }
    if !mounted.is_null() {
        unsafe { vfs_node_unref(mounted) };
    }

    // SAFETY: `dir->ops` is null or a live ops table.  The filesystem frees the
    // inode itself when the last link (and any open handle) goes away.
    let ops = unsafe { (*dir).ops };
    if !ops.is_null() {
        if let Some(unlink) = unsafe { (*ops).unlink } {
            let ret = unsafe { unlink(dir, name) };
            if ret == 0 {
                unsafe { crate::dcache::invalidate(dir, name) };
            }
            return ret;
        }
        if let Some(delete) = unsafe { (*ops).delete } {
            let ret = unsafe { delete(dir, name) };
            if ret == 0 {
                unsafe { crate::dcache::invalidate(dir, name) };
            }
            return ret;
        }
    }
    -1
}

// ── Directory listing (was `listdir_vfs` in vfs_ops.c) ──────────────────

#[no_mangle]
pub unsafe extern "C" fn listdir_vfs(dir: *mut VfsNode) {
    if dir.is_null() || unsafe { (*dir).type_ } != VFS_DIRECTORY {
        return;
    }
    // SAFETY: `dir->ops` is null or a live ops table.
    let ops = unsafe { (*dir).ops };
    if !ops.is_null() {
        if let Some(listdir) = unsafe { (*ops).listdir } {
            unsafe { listdir(dir) };
        }
    }

    unsafe { cact_sync::mutex_lock(vfs_mutex()) };
    // SAFETY: indices stay below `MOUNT_COUNT <= VFS_MOUNT_MAX`.
    unsafe {
        for i in 0..MOUNT_COUNT {
            let m = &*ptr::addr_of!(MOUNT_TABLE[i]);
            if m.host == dir {
                printk(b"  \0".as_ptr() as *const c_char);
                printk(m.name.as_ptr());
                printk(b"/\n\0".as_ptr() as *const c_char);
            }
        }
        cact_sync::mutex_unlock(vfs_mutex());
    }
}

// ── Mount-tree rendering (/proc/mounts) ─────────────────────────────────

/// Build the mount path of the filesystem rooted at `host` into `out`.
/// Walks up the mount tree (a mount's `host` may itself be another mount's
/// target) so `/usr/bin` is reconstructed as `usrfs` → `binfs`.
unsafe fn mount_host_path(host: *mut VfsNode, out: *mut c_char, max: usize, depth: i32) -> usize {
    if host.is_null() || host == unsafe { vfs_root } || depth > 16 {
        if max > 0 {
            unsafe { *out = 0 };
        }
        return 0;
    }
    // SAFETY: indices stay below `MOUNT_COUNT <= VFS_MOUNT_MAX`.
    unsafe {
        for j in 0..MOUNT_COUNT {
            let m = &*ptr::addr_of!(MOUNT_TABLE[j]);
            if m.target == host {
                let n = mount_host_path(m.host, out, max, depth + 1);
                let mut p = n;
                if p + 1 < max {
                    *out.add(p) = b'/' as c_char;
                    p += 1;
                }
                let mut k = 0usize;
                while m.name[k] != 0 && p + 1 < max {
                    *out.add(p) = m.name[k];
                    p += 1;
                    k += 1;
                }
                *out.add(p) = 0;
                return p;
            }
        }
        if max > 0 {
            *out = 0;
        }
    }
    0
}

/// Append a NUL-terminated C string to `(dst, cap)` at offset `p`; returns the
/// new offset.  Never writes past `cap - 1` bytes.
unsafe fn append_cstr(dst: *mut c_char, cap: usize, p: usize, s: *const c_char) -> usize {
    if cap == 0 {
        return p;
    }
    let mut p = p;
    let mut i = 0usize;
    // SAFETY: `s` is NUL-terminated and `dst` has `cap` writable bytes.
    unsafe {
        while *s.add(i) != 0 && p + 1 < cap {
            *dst.add(p) = *s.add(i);
            p += 1;
            i += 1;
        }
    }
    p
}

/// Render the mount tree as /proc/mounts lines ("<src> <path> <fstype> rw 0 0").
/// Our mounts have no backing device, so `<src>` repeats the filesystem type.
/// Returns the number of bytes written (excluding the trailing NUL).
#[no_mangle]
pub unsafe extern "C" fn vfs_render_mounts(buf: *mut c_char, cap: i32) -> i32 {
    if buf.is_null() || cap <= 0 {
        return 0;
    }
    let cap = cap as usize;
    let mut p = 0usize;
    // SAFETY: every helper bounds its writes by `cap`.
    unsafe {
        for i in 0..MOUNT_COUNT {
            let m = &*ptr::addr_of!(MOUNT_TABLE[i]);

            // Synthetic symlinks are stored as mounts internally; they are not
            // real mounts and must not appear in /proc/mounts.
            if !m.target.is_null() && (*m.target).type_ == VFS_SYMLINK {
                continue;
            }

            let mut path = [0 as c_char; 256];
            let mut pl = mount_host_path(m.host, path.as_mut_ptr(), 256, 0);
            if pl + 1 < 256 {
                *path.as_mut_ptr().add(pl) = b'/' as c_char;
                pl += 1;
            }
            let mut k = 0usize;
            while m.name[k] != 0 && pl + 1 < 256 {
                *path.as_mut_ptr().add(pl) = m.name[k];
                pl += 1;
                k += 1;
            }
            *path.as_mut_ptr().add(pl) = 0;

            let src = if m.fstype[0] != 0 {
                m.fstype.as_ptr()
            } else {
                b"none\0".as_ptr() as *const c_char
            };

            p = append_cstr(buf, cap, p, src);
            p = append_cstr(buf, cap, p, b" \0".as_ptr() as *const c_char);
            p = append_cstr(buf, cap, p, path.as_ptr());
            p = append_cstr(buf, cap, p, b" \0".as_ptr() as *const c_char);
            p = append_cstr(buf, cap, p, src);
            p = append_cstr(buf, cap, p, b" rw,relatime 0 0\n\0".as_ptr() as *const c_char);
        }
        if p < cap {
            *buf.add(p) = 0;
        }
    }
    p as i32
}

// ── statfs ──────────────────────────────────────────────────────────────

#[inline]
unsafe fn type_hash(name: *const c_char) -> u32 {
    let mut h: u32 = 5381;
    let mut i = 0usize;
    // SAFETY: `name` is a NUL-terminated C string.
    unsafe {
        let mut c = *name;
        while c != 0 {
            h = h.wrapping_mul(33).wrapping_add(c as u8 as u32);
            i += 1;
            c = *name.add(i);
        }
    }
    h
}

/// Filesystem type of the mount that backs `node`: walk up the physical parent
/// chain (via the dcache parent table) until a node is a mount target.
unsafe fn mount_fstype_of(node: *mut VfsNode) -> *const c_char {
    let mut cur = node;
    let mut guard = 0;
    while !cur.is_null() && guard < 64 {
        unsafe { cact_sync::mutex_lock(vfs_mutex()) };
        let mut found: *const c_char = ptr::null();
        // SAFETY: indices stay below `MOUNT_COUNT <= VFS_MOUNT_MAX`.
        unsafe {
            for i in 0..MOUNT_COUNT {
                let m = &*ptr::addr_of!(MOUNT_TABLE[i]);
                if m.target == cur {
                    found = m.fstype.as_ptr();
                    break;
                }
            }
            cact_sync::mutex_unlock(vfs_mutex());
        }
        if !found.is_null() {
            return found;
        }
        let p = unsafe { crate::dcache::parent_of(cur) };
        if p.is_null() || p == cur {
            break;
        }
        cur = p;
        guard += 1;
    }
    ptr::null()
}

/// Fill a `cact_statfs_t` for the filesystem containing `node`.
#[no_mangle]
pub unsafe extern "C" fn vfs_fill_statfs(node: *mut VfsNode, buf: *mut CactStatfs) {
    if buf.is_null() {
        return;
    }
    // SAFETY: `buf` is a live 32-byte out-struct; `mount_fstype_of` reads the
    // static mount table under `vfs_mutex`.
    unsafe {
        let fst = mount_fstype_of(node);
        let name = if fst.is_null() {
            b"none\0".as_ptr() as *const c_char
        } else {
            fst
        };
        (*buf).f_type = type_hash(name);
        (*buf).f_namelen = 255;

        let mut prof = [0u32; 6];
        if crate::sb::sb_lookup(name, prof.as_mut_ptr()) {
            (*buf).f_bsize = prof[0];
            (*buf).f_blocks = prof[1];
            (*buf).f_bfree = prof[2];
            (*buf).f_bavail = prof[3];
            (*buf).f_files = prof[4];
            (*buf).f_ffree = prof[5];
        } else {
            // RAM-filesystem profile (pseudo filesystems report zero blocks).
            (*buf).f_bsize = 4096;
            (*buf).f_blocks = 0;
            (*buf).f_bfree = 0;
            (*buf).f_bavail = 0;
            (*buf).f_files = 0;
            (*buf).f_ffree = 0;
        }
    }
}
