//! Generic VFS operation wrappers, ported from `Cact/fs/vfs/vfs_ops.c`.
//!
//! These are the entry points the C syscall layer and the C filesystems call:
//! plain I/O, the file-aware (per-open `fops`) variants, directory ops, the
//! POSIX permission check, the `truncate/chmod/...` dispatch and the mmap
//! backing resolver.  Dispatch is a plain table call on the node's `vfs_ops_t`.

use core::ffi::{c_char, c_void};
use core::ptr;

use cact_sync::TaskStruct;

use crate::abi::*;
use crate::file::vfs_fill_stat;
use crate::vfs::finddir_vfs;

unsafe extern "C" {
    fn memfd_node_handle(node: *mut VfsNode) -> i32;
    fn cact_current_task_get() -> *mut TaskStruct;
    fn vfs_as_flush(owner: *mut c_void, ino: u32, off: u32, size: u32);
}

/// Flush a file-backed mapping's page-cache range, called on munmap.  Resolves
/// the fd to its node and flushes `(node->priv, node->inode)`.
#[no_mangle]
pub unsafe extern "C" fn vfs_as_flush_fd(fd: i32, off: u32, size: u32) {
    if fd < 0 {
        return;
    }
    // SAFETY: reads the current task's fd table and the node behind the fd.
    unsafe {
        let t = cact_current_task_get();
        if t.is_null() {
            return;
        }
        let proc = (*t).proc;
        if proc.is_null() {
            return;
        }
        let fds = (*proc).fds;
        if fds.is_null() {
            return;
        }
        let files = fds as *mut *mut File;
        let f = *files.add(fd as usize);
        if f.is_null() {
            return;
        }
        let node = (*f).node;
        if node.is_null() {
            return;
        }
        vfs_as_flush((*node).priv_, (*node).inode, off, size);
    }
}

/// Must match `MAX_FD` in task.h.
const MAX_FD: i32 = 256;

// ── Generic I/O dispatch ────────────────────────────────────────────────

#[no_mangle]
pub unsafe extern "C" fn read_vfs(
    node: *mut VfsNode,
    off: u32,
    size: u32,
    buf: *mut c_char,
) -> i32 {
    if node.is_null() {
        return -1;
    }
    // SAFETY: `node->ops` is null or a live ops table.
    let ops = unsafe { (*node).ops };
    if ops.is_null() {
        return -1;
    }
    match unsafe { (*ops).read } {
        Some(read) => unsafe { read(node, off, size, buf) },
        None => -1,
    }
}

#[no_mangle]
pub unsafe extern "C" fn write_vfs(
    node: *mut VfsNode,
    off: u32,
    size: u32,
    buf: *mut c_char,
) -> i32 {
    if node.is_null() {
        return -1;
    }
    let ops = unsafe { (*node).ops };
    if ops.is_null() {
        return -1;
    }
    match unsafe { (*ops).write } {
        Some(write) => unsafe { write(node, off, size, buf) },
        None => -1,
    }
}

#[no_mangle]
pub unsafe extern "C" fn open_vfs(node: *mut VfsNode) {
    if node.is_null() {
        return;
    }
    let ops = unsafe { (*node).ops };
    if !ops.is_null() {
        if let Some(open) = unsafe { (*ops).open } {
            unsafe { open(node) };
        }
    }
}

#[no_mangle]
pub unsafe extern "C" fn close_vfs(node: *mut VfsNode) {
    if node.is_null() {
        return;
    }
    let ops = unsafe { (*node).ops };
    if !ops.is_null() {
        if let Some(close) = unsafe { (*ops).close } {
            unsafe { close(node) };
        }
    }
}

#[no_mangle]
pub unsafe extern "C" fn ioctl_vfs(node: *mut VfsNode, cmd: u32, arg: *mut c_void) -> i32 {
    if node.is_null() {
        return -1;
    }
    let ops = unsafe { (*node).ops };
    if ops.is_null() {
        return -1;
    }
    match unsafe { (*ops).ioctl } {
        Some(ioctl) => unsafe { ioctl(node, cmd, arg) },
        None => -1,
    }
}

// ── File-aware wrappers ─────────────────────────────────────────────────
//
// A node with fops keeps per-open state in file_t.priv; the syscall layer
// reaches it through these wrappers.  Nodes without fops behave exactly as
// the node-level wrappers above.

#[no_mangle]
pub unsafe extern "C" fn read_file_vfs(
    f: *mut File,
    off: u32,
    size: u32,
    buf: *mut c_char,
) -> i32 {
    if f.is_null() || unsafe { (*f).node }.is_null() {
        return -1;
    }
    // SAFETY: `f` and its node are live.
    unsafe {
        let node = (*f).node;
        let fops = (*node).fops as *mut VfsFileOps;
        if !fops.is_null() {
            if let Some(read) = (*fops).read {
                return read(node, (*f).priv_, off, size, buf);
            }
        }
        read_vfs(node, off, size, buf)
    }
}

#[no_mangle]
pub unsafe extern "C" fn write_file_vfs(
    f: *mut File,
    off: u32,
    size: u32,
    buf: *mut c_char,
) -> i32 {
    if f.is_null() || unsafe { (*f).node }.is_null() {
        return -1;
    }
    unsafe {
        let node = (*f).node;
        let fops = (*node).fops as *mut VfsFileOps;
        if !fops.is_null() {
            if let Some(write) = (*fops).write {
                return write(node, (*f).priv_, off, size, buf);
            }
        }
        write_vfs(node, off, size, buf)
    }
}

#[no_mangle]
pub unsafe extern "C" fn ioctl_file_vfs(f: *mut File, cmd: u32, arg: *mut c_void) -> i32 {
    if f.is_null() || unsafe { (*f).node }.is_null() {
        return -1;
    }
    unsafe {
        let node = (*f).node;
        let fops = (*node).fops as *mut VfsFileOps;
        if !fops.is_null() {
            if let Some(ioctl) = (*fops).ioctl {
                return ioctl(node, (*f).priv_, cmd, arg);
            }
        }
        ioctl_vfs(node, cmd, arg)
    }
}

#[no_mangle]
pub unsafe extern "C" fn poll_file_vfs(f: *mut File, events: u32) -> i32 {
    if f.is_null() || unsafe { (*f).node }.is_null() {
        return 0;
    }
    unsafe {
        let node = (*f).node;
        let fops = (*node).fops as *mut VfsFileOps;
        if !fops.is_null() {
            if let Some(poll) = (*fops).poll {
                return poll(node, (*f).priv_, events);
            }
        }
        poll_vfs(node, events)
    }
}

// Resolve an open fd + mapping offset to the shared backing object that holds
// the bytes, if the node has one.  `do_mmap` uses this to install the object's
// own frames instead of copying the file through read().
//
// Two sources, in order:
//   1. node->ops->mmap_backing — self-describing nodes (a DRM card node
//      handing out GEM buffers at faked mmap offsets, …).
//   2. memfd — a memfd maps its own storage, so the object offset is the
//      file offset.
// Returns 0 with *backing_out > 0 on success, -1 when nothing backs the fd.
#[no_mangle]
pub unsafe extern "C" fn vfs_mmap_resolve(
    fd: i32,
    off: u32,
    len: u32,
    backing_out: *mut i32,
    obj_off_out: *mut u32,
) -> i32 {
    if backing_out.is_null() || obj_off_out.is_null() {
        return -1;
    }
    // SAFETY: both out-pointers are non-null and writable.
    unsafe {
        *backing_out = 0;
        *obj_off_out = 0;
    }
    if fd < 0 || fd >= MAX_FD {
        return -1;
    }
    // SAFETY: the scheduler accessor returns this CPU's task or null; `proc`
    // and its `fds` table are owned by that task.
    unsafe {
        let t = cact_current_task_get();
        if t.is_null() {
            return -1;
        }
        let proc = (*t).proc;
        if proc.is_null() {
            return -1;
        }
        let fds = (*proc).fds;
        if fds.is_null() {
            return -1;
        }
        // `task_fd_table_t.files` is the first member (offset 0); read it as a
        // raw pointer array so the differing Rust/C mirror type is irrelevant.
        let files = fds as *mut *mut File;
        let f = *files.add(fd as usize);
        if f.is_null() {
            return -1;
        }
        let node = (*f).node;
        if node.is_null() {
            return -1;
        }

        let ops = (*node).ops;
        if !ops.is_null() {
            if let Some(mmap_backing) = (*ops).mmap_backing {
                let rc = mmap_backing(node, off, len, backing_out, obj_off_out);
                if rc == 0 && *backing_out > 0 {
                    return 0;
                }
                *backing_out = 0;
            }
        }

        let h = memfd_node_handle(node);
        if h > 0 {
            *backing_out = h;
            *obj_off_out = off;
            return 0;
        }
        -1
    }
}

// ── Directory operations ────────────────────────────────────────────────

#[no_mangle]
pub unsafe extern "C" fn readdir_vfs(dir: *mut VfsNode, index: u32) -> *mut VfsDirent {
    if dir.is_null() || unsafe { (*dir).type_ } != VFS_DIRECTORY {
        return ptr::null_mut();
    }
    let ops = unsafe { (*dir).ops };
    if ops.is_null() {
        return ptr::null_mut();
    }
    match unsafe { (*ops).readdir } {
        Some(readdir) => unsafe { readdir(dir, index) },
        None => ptr::null_mut(),
    }
}

#[no_mangle]
pub unsafe extern "C" fn create_vfs(dir: *mut VfsNode, name: *const c_char) -> i32 {
    if dir.is_null() || unsafe { (*dir).type_ } != VFS_DIRECTORY {
        return -1;
    }
    let ops = unsafe { (*dir).ops };
    if ops.is_null() {
        return -1;
    }
    match unsafe { (*ops).create } {
        Some(create) => {
            let ret = unsafe { create(dir, name) };
            if ret == 0 {
                unsafe { crate::dcache::invalidate(dir, name) };
            }
            ret
        }
        None => -1,
    }
}

#[no_mangle]
pub unsafe extern "C" fn delete_vfs(dir: *mut VfsNode, name: *const c_char) -> i32 {
    if dir.is_null() || unsafe { (*dir).type_ } != VFS_DIRECTORY {
        return -1;
    }
    let ops = unsafe { (*dir).ops };
    if ops.is_null() {
        return -1;
    }
    match unsafe { (*ops).delete } {
        Some(delete) => {
            let ret = unsafe { delete(dir, name) };
            if ret == 0 {
                unsafe { crate::dcache::invalidate(dir, name) };
            }
            ret
        }
        None => -1,
    }
}

#[no_mangle]
pub unsafe extern "C" fn mkdir_vfs(dir: *mut VfsNode, name: *const c_char) -> i32 {
    if dir.is_null() || unsafe { (*dir).type_ } != VFS_DIRECTORY {
        return -1;
    }
    let ops = unsafe { (*dir).ops };
    if ops.is_null() {
        return -1;
    }
    match unsafe { (*ops).mkdir } {
        Some(mkdir) => {
            let ret = unsafe { mkdir(dir, name) };
            if ret == 0 {
                unsafe { crate::dcache::invalidate(dir, name) };
            }
            ret
        }
        None => -1,
    }
}

#[no_mangle]
pub unsafe extern "C" fn rmdir_vfs(dir: *mut VfsNode, name: *const c_char) -> i32 {
    if dir.is_null() || unsafe { (*dir).type_ } != VFS_DIRECTORY {
        return -1;
    }
    let ops = unsafe { (*dir).ops };
    if ops.is_null() {
        return -1;
    }
    match unsafe { (*ops).rmdir } {
        Some(rmdir) => {
            let ret = unsafe { rmdir(dir, name) };
            if ret == 0 {
                unsafe { crate::dcache::invalidate(dir, name) };
            }
            ret
        }
        None => -1,
    }
}

#[no_mangle]
pub unsafe extern "C" fn rename_vfs(
    dir: *mut VfsNode,
    oldname: *const c_char,
    newname: *const c_char,
) -> i32 {
    if dir.is_null() || unsafe { (*dir).type_ } != VFS_DIRECTORY {
        return -1;
    }
    let ops = unsafe { (*dir).ops };
    if ops.is_null() {
        return -1;
    }
    match unsafe { (*ops).rename } {
        Some(rename) => {
            let ret = unsafe { rename(dir, oldname, newname) };
            if ret == 0 {
                unsafe {
                    crate::dcache::invalidate(dir, oldname);
                    crate::dcache::invalidate(dir, newname);
                }
            }
            ret
        }
        None => -1,
    }
}

// ── Permissions ─────────────────────────────────────────────────────────

// POSIX rwx permission check.
// NOTE: this is a TOCTOU window — the node's mode/uid/gid or the current task's
// credentials could change between the check and the VFS operation.  Callers
// should hold vfs_mutex (or equivalent) across check + operation.
#[no_mangle]
pub unsafe extern "C" fn vfs_check_perm(node: *mut VfsNode, perm: u32) -> i32 {
    if node.is_null() {
        return -1;
    }
    // SAFETY: `node` is a live VfsNode.
    unsafe {
        let mode = (*node).mode;

        // No mode set → allow everything.
        if mode == 0 {
            return 0;
        }

        let t = cact_current_task_get();
        // Kernel tasks bypass permission checks.
        if t.is_null() || (*t).is_kernel != 0 {
            return 0;
        }

        let proc = (*t).proc;
        // Root (euid=0) bypasses permission checks.
        if !proc.is_null() && (*proc).euid == 0 {
            return 0;
        }

        let shift = if !proc.is_null() && (*proc).euid == (*node).uid {
            6 // owner
        } else if !proc.is_null() && (*proc).egid == (*node).gid {
            3 // group
        } else {
            0 // other
        };

        let allowed = (mode >> shift) & 0x07;
        if (allowed & perm) == perm {
            0
        } else {
            -1
        }
    }
}

// ── truncate / chmod / chown / mknod / stat / poll / lseek ──────────────

#[no_mangle]
pub unsafe extern "C" fn truncate_vfs(node: *mut VfsNode, length: u32) -> i32 {
    if node.is_null() {
        return -1;
    }
    // SAFETY: `node` is live.
    unsafe {
        let ops = (*node).ops;
        if !ops.is_null() {
            if let Some(truncate) = (*ops).truncate {
                return truncate(node, length);
            }
        }
        (*node).size = length;
        0
    }
}

#[no_mangle]
pub unsafe extern "C" fn chmod_vfs(node: *mut VfsNode, mode: u32) -> i32 {
    if node.is_null() {
        return -1;
    }
    unsafe {
        let ops = (*node).ops;
        if !ops.is_null() {
            if let Some(chmod) = (*ops).chmod {
                return chmod(node, mode);
            }
        }
        (*node).mode = mode & 0o777;
        0
    }
}

#[no_mangle]
pub unsafe extern "C" fn chown_vfs(node: *mut VfsNode, uid: u32, gid: u32) -> i32 {
    if node.is_null() {
        return -1;
    }
    unsafe {
        let ops = (*node).ops;
        if !ops.is_null() {
            if let Some(chown) = (*ops).chown {
                return chown(node, uid, gid);
            }
        }
        if uid != u32::MAX {
            (*node).uid = uid;
        }
        if gid != u32::MAX {
            (*node).gid = gid;
        }
        0
    }
}

#[no_mangle]
pub unsafe extern "C" fn mknod_vfs(
    dir: *mut VfsNode,
    name: *const c_char,
    mode: u32,
    dev: u32,
) -> i32 {
    if dir.is_null() || unsafe { (*dir).type_ } != VFS_DIRECTORY || name.is_null() {
        return -1;
    }
    unsafe {
        let ops = (*dir).ops;
        if !ops.is_null() {
            if let Some(mknod) = (*ops).mknod {
                return mknod(dir, name, mode, dev);
            }
        }
        if ops.is_null() {
            return -1;
        }
        let create = match (*ops).create {
            Some(create) => create,
            None => return -1,
        };
        let ret = create(dir, name);
        if ret < 0 {
            return -1;
        }
        let node = finddir_vfs(dir, name);
        if node.is_null() {
            return -1;
        }
        if (mode & 0xF000) == 0x2000 {
            (*node).type_ = VFS_CHARDEVICE;
        } else if (mode & 0xF000) == 0x6000 {
            (*node).type_ = VFS_BLOCKDEVICE;
        }
        (*node).mode = mode & 0o777;
        0
    }
}

#[no_mangle]
pub unsafe extern "C" fn stat_vfs(node: *mut VfsNode, buf: *mut u32) -> i32 {
    if node.is_null() || buf.is_null() {
        return -1;
    }
    // SAFETY: `node` and a 4-word `buf` are live.
    unsafe {
        let ops = (*node).ops;
        if !ops.is_null() {
            if let Some(stat) = (*ops).stat {
                return stat(node, buf);
            }
        }
        vfs_fill_stat(node, buf);
        0
    }
}

#[no_mangle]
pub unsafe extern "C" fn poll_vfs(node: *mut VfsNode, events: u32) -> i32 {
    if node.is_null() {
        return VFS_POLLNVAL as i32;
    }
    // SAFETY: `node` is live.
    unsafe {
        let ops = (*node).ops;
        if !ops.is_null() {
            if let Some(poll) = (*ops).poll {
                return poll(node, events);
            }
        }
        // Default: regular files and dirs are always ready.
        let mut revents = 0u32;
        if events & VFS_POLLIN != 0 {
            revents |= VFS_POLLIN;
        }
        if events & VFS_POLLOUT != 0 {
            revents |= VFS_POLLOUT;
        }
        revents as i32
    }
}

#[no_mangle]
pub unsafe extern "C" fn lseek_vfs(
    node: *mut VfsNode,
    offset: i32,
    whence: i32,
    result: *mut u32,
) -> i32 {
    if node.is_null() || result.is_null() {
        return -1;
    }
    // SAFETY: `node` is live and `result` is writable.
    unsafe {
        let ops = (*node).ops;
        if !ops.is_null() {
            if let Some(lseek) = (*ops).lseek {
                return lseek(node, offset, whence, result);
            }
        }
        // Default: files always support seek — caller handles default logic.
        -1
    }
}
