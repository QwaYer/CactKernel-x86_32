//! File-description objects, path resolution and stat helpers, ported from
//! `Cact/fs/vfs/vfs_file.c`.
//!
//! `file_alloc`/`file_free` own the `file_t` lifecycle (per-open state, shared
//! by `dup()`); the `vfs_resolve_*` family canonicalises a path against
//! `current_task->proc->cwd` and walks it through the Rust core; the `*_vfs`
//! stat helpers format the 4-word buffer C expects.

use core::ffi::c_char;
use core::mem::size_of;
use core::ptr;

use cact_sync::{ProcMeta, TaskStruct};

use crate::abi::*;
use crate::vfs::{strlcpy, vfs_root, vfs_walk_path_follow};

unsafe extern "C" {
    fn printk(s: *const c_char);
    fn cact_current_task_get() -> *mut TaskStruct;
}

// ── current process access ──────────────────────────────────────────────

/// `current_task->proc`, or null when there is no running task.
unsafe fn current_proc() -> *mut ProcMeta {
    // SAFETY: the scheduler's per-CPU accessor returns this CPU's live task or
    // null; a non-null task's `proc` is either null or an owned `ProcMeta`.
    unsafe {
        let t = cact_current_task_get();
        if t.is_null() {
            return ptr::null_mut();
        }
        (*t).proc
    }
}

// ── file description lifecycle ──────────────────────────────────────────

#[no_mangle]
pub unsafe extern "C" fn file_alloc(node: *mut VfsNode) -> *mut File {
    if node.is_null() {
        return ptr::null_mut();
    }
    // SAFETY: `kmalloc` returns a block of exactly `size_of::<File>()` bytes or
    // null; we fully initialise it before returning it to the caller.
    let f = cact_mm::kmalloc(size_of::<File>() as u32) as *mut File;
    if f.is_null() {
        unsafe {
            printk(b"  vfs         : cannot allocate file\n\0".as_ptr() as *const c_char);
        }
        return ptr::null_mut();
    }
    unsafe {
        (*f).node = node;
        (*f).offset = 0;
        (*f).flags = 0;
        (*f).cloexec = 0;
        (*f).refcount = 1;
        (*f).priv_ = ptr::null_mut();
    }

    // node->ops->open(node)
    let ops = unsafe { (*node).ops };
    if !ops.is_null() {
        if let Some(open) = unsafe { (*ops).open } {
            unsafe { open(node) };
        }
    }

    // Per-open node state (DRM client, …): one instance per open(), shared by
    // dup()ed descriptors because they share this file_t.
    let fops = unsafe { (*node).fops } as *mut VfsFileOps;
    if !fops.is_null() {
        if let Some(open) = unsafe { (*fops).open } {
            unsafe { open(node, f) };
        }
    }
    f
}

#[no_mangle]
pub unsafe extern "C" fn file_free(f: *mut File) {
    if f.is_null() {
        return;
    }
    // SAFETY: `f` is a live file description owned by the caller.
    unsafe {
        let node = (*f).node;
        if !node.is_null() {
            let fops = (*node).fops as *mut VfsFileOps;
            if !fops.is_null() {
                if let Some(release) = (*fops).release {
                    release(node, f);
                }
            }
            let ops = (*node).ops;
            if !ops.is_null() {
                if let Some(close) = (*ops).close {
                    close(node);
                }
            }
        }
        cact_mm::kfree(f as *mut u8);
    }
}

#[no_mangle]
pub unsafe extern "C" fn file_ref(f: *mut File) -> *mut File {
    if !f.is_null() {
        unsafe { (*f).refcount += 1 };
    }
    f
}

#[no_mangle]
pub unsafe extern "C" fn file_unref(f: *mut File) -> i32 {
    if f.is_null() {
        return -1;
    }
    // SAFETY: `f` is a live file description.
    unsafe {
        if (*f).refcount == 0 {
            return -1;
        }
        (*f).refcount -= 1;
        if (*f).refcount == 0 {
            file_free(f);
            return 0;
        }
        (*f).refcount as i32
    }
}

// ── Path canonicalisation and resolution ────────────────────────────────

#[no_mangle]
pub unsafe extern "C" fn vfs_make_abs(path: *const c_char, abs: *mut c_char, abs_max: i32) {
    // SAFETY: `path` is NUL-terminated, `abs` has `abs_max` writable bytes, and
    // `current_proc`'s `cwd` is a 256-byte NUL-terminated buffer.
    unsafe {
        let max = abs_max as usize;
        let mut p = 0usize;
        if *path != b'/' as c_char {
            let proc = current_proc();
            if !proc.is_null() {
                let cwd = &(*proc).cwd;
                let mut i = 0usize;
                while i < cwd.len() && cwd[i] != 0 && p < max - 2 {
                    *abs.add(p) = cwd[i] as c_char;
                    p += 1;
                    i += 1;
                }
                if p > 0 && *abs.add(p - 1) != b'/' as c_char {
                    *abs.add(p) = b'/' as c_char;
                    p += 1;
                }
            }
        }
        let mut i = 0usize;
        while *path.add(i) != 0 && p < max - 1 {
            *abs.add(p) = *path.add(i);
            p += 1;
            i += 1;
        }
        *abs.add(p) = 0;
    }
}

/// Canonicalise an absolute path: collapse `/./`, resolve `/../`, remove
/// duplicated slashes.
unsafe fn canon_abs(path: *const c_char, out: *mut c_char, out_max: i32) {
    // SAFETY: writes stay within the 512-byte `abs`/`out` buffers and the
    // fixed-size segment tables.
    unsafe {
        let mut abs = [0 as c_char; 512];
        vfs_make_abs(path, abs.as_mut_ptr(), 512);

        let mut seg_start = [0i32; 128];
        let mut seg_len = [0i32; 128];
        let mut nseg = 0usize;
        let base = abs.as_ptr();
        let mut s = base;
        while *s != 0 {
            while *s == b'/' as c_char {
                s = s.add(1);
            }
            if *s == 0 {
                break;
            }
            let seg = s;
            let mut slen = 0usize;
            while *s != 0 && *s != b'/' as c_char {
                s = s.add(1);
                slen += 1;
            }
            if slen == 1 && *seg == b'.' as c_char {
                continue;
            }
            // ".." is kept as an ordinary segment: the walker resolves it
            // against the physical parent (mounts and symlinks included),
            // rather than collapsing it lexically here.
            if nseg >= 128 {
                break;
            }
            seg_start[nseg] = (seg as usize - base as usize) as i32;
            seg_len[nseg] = slen as i32;
            nseg += 1;
        }

        let max = out_max as usize;
        let mut p = 0usize;
        let mut i = 0usize;
        while i < nseg && p < max - 2 {
            *out.add(p) = b'/' as c_char;
            p += 1;
            let mut j = 0usize;
            while j < seg_len[i] as usize && p < max - 1 {
                *out.add(p) = abs[seg_start[i] as usize + j];
                p += 1;
                j += 1;
            }
            i += 1;
        }
        if p == 0 {
            *out.add(p) = b'/' as c_char;
            p += 1;
        }
        *out.add(p) = 0;
    }
}

#[no_mangle]
pub unsafe extern "C" fn vfs_resolve_path(path: *const c_char) -> *mut VfsNode {
    if path.is_null() || unsafe { current_proc() }.is_null() {
        return ptr::null_mut();
    }
    // SAFETY: `abs` is a 512-byte stack buffer; the walk follows the result.
    unsafe {
        let mut abs = [0 as c_char; 512];
        canon_abs(path, abs.as_mut_ptr(), 512);
        vfs_walk_path_follow(vfs_root, abs.as_ptr(), ptr::null_mut())
    }
}

#[no_mangle]
pub unsafe extern "C" fn vfs_resolve_parent_follow(
    path: *const c_char,
    basename_out: *mut c_char,
    basename_max: i32,
) -> *mut VfsNode {
    let proc = unsafe { current_proc() };
    if path.is_null() || proc.is_null() {
        return ptr::null_mut();
    }

    // SAFETY: `abs` is a 512-byte stack buffer; basename copy is bounded by
    // `basename_max`; `cwd` is a 256-byte NUL-terminated buffer.
    unsafe {
        let mut abs = [0 as c_char; 512];
        canon_abs(path, abs.as_mut_ptr(), 512);

        let mut last_slash: i32 = -1;
        let mut i = 0usize;
        while *abs.as_ptr().add(i) != 0 {
            if *abs.as_ptr().add(i) == b'/' as c_char {
                last_slash = i as i32;
            }
            i += 1;
        }

        let bmax = basename_max as usize;
        if last_slash <= 0 {
            let bn = if last_slash == 0 {
                abs.as_ptr().add(1)
            } else {
                path
            };
            let mut k = 0usize;
            while *bn.add(k) != 0 && k + 1 < bmax {
                *basename_out.add(k) = *bn.add(k);
                k += 1;
            }
            *basename_out.add(k) = 0;
            if last_slash == 0 {
                return vfs_root;
            }
            return vfs_walk_path_follow(
                vfs_root,
                (*proc).cwd.as_ptr() as *const c_char,
                ptr::null_mut(),
            );
        }

        let bn = abs.as_ptr().add(last_slash as usize + 1);
        let mut k = 0usize;
        while *bn.add(k) != 0 && k + 1 < bmax {
            *basename_out.add(k) = *bn.add(k);
            k += 1;
        }
        *basename_out.add(k) = 0;

        let mut parent_path = [0 as c_char; 512];
        let ls = last_slash as usize;
        let mut j = 0usize;
        while j < ls && j < 511 {
            parent_path[j] = abs[j];
            j += 1;
        }
        parent_path[ls] = 0;
        vfs_walk_path_follow(vfs_root, parent_path.as_ptr(), ptr::null_mut())
    }
}

#[no_mangle]
pub unsafe extern "C" fn vfs_resolve_parent(
    path: *const c_char,
    basename_out: *mut c_char,
    basename_max: i32,
) -> *mut VfsNode {
    unsafe { vfs_resolve_parent_follow(path, basename_out, basename_max) }
}

// ── stat helpers ────────────────────────────────────────────────────────

#[no_mangle]
pub extern "C" fn vfs_type_to_mode(type_: u32) -> u32 {
    match type_ {
        VFS_FILE => 0x8000,        // S_IFREG
        VFS_DIRECTORY => 0x4000,   // S_IFDIR
        VFS_CHARDEVICE => 0x2000,  // S_IFCHR
        VFS_BLOCKDEVICE => 0x6000, // S_IFBLK
        VFS_PIPE => 0x1000,        // S_IFIFO
        VFS_SYMLINK => 0xA000,     // S_IFLNK
        _ => 0,
    }
}

#[no_mangle]
pub unsafe extern "C" fn vfs_fill_stat(node: *mut VfsNode, buf: *mut u32) {
    // SAFETY: `node` and `buf` are live and `buf` has 4 writable words.
    unsafe {
        *buf = (*node).inode;
        // st_mode is "type bits | rwxrwxrwx" (vfs.h) — include the permission
        // bits, which the original dropped, so `stat`/`ls -l` show them.
        *buf.add(1) = vfs_type_to_mode((*node).type_) | ((*node).mode & 0o777);
        *buf.add(2) = (*node).size;
        *buf.add(3) = (*node).type_;
    }
}

/// Fill the rich `cact_statx_t` from a node.
#[no_mangle]
pub unsafe extern "C" fn vfs_fill_statx(node: *mut VfsNode, buf: *mut CactStatx) {
    if node.is_null() || buf.is_null() {
        return;
    }
    // SAFETY: `node` and `buf` are live; every field is a plain u32.
    unsafe {
        (*buf).ino = (*node).inode;
        (*buf).mode = vfs_type_to_mode((*node).type_) | ((*node).mode & 0o777);
        (*buf).nlink = (*node).refcount;
        (*buf).uid = (*node).uid;
        (*buf).gid = (*node).gid;
        (*buf).size = (*node).size;
        (*buf).blksize = 4096;
        (*buf).blocks = ((*node).size + 511) / 512;
        (*buf).atime = 0;
        (*buf).mtime = 0;
        (*buf).ctime = 0;
        (*buf).type_ = (*node).type_;
    }
}

#[no_mangle]
pub unsafe extern "C" fn vfs_strlcpy(dst: *mut c_char, src: *const c_char, max: i32) {
    // SAFETY: forwarded to the shared helper, which writes at most `max` bytes.
    unsafe { strlcpy(dst, src, max as usize) };
}
