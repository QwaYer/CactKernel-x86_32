//! Pipes, ported from `Cact/fs/pipe/pipe.c` and corrected.
//!
//! The C original double-counted open ends (`pipe_create` set both counters to
//! 1 *and* `file_alloc`'s `open()` incremented them again), so neither
//! `read_open` nor `write_open` ever reached zero — EOF on the read end and
//! EPIPE/SIGPIPE on the write end never fired, and the `pipe_t` (plus its two
//! node wrappers) leaked on every close.
//!
//! Here the accounting is clean: `open()`/`close()` own the open-end counters,
//! `p.ref_count` counts live node wrappers (freed with the pipe when it hits
//! zero), and a node's `refcount` is its open count.  Behaviour otherwise
//! matches the original (ring buffer, blocking via `sched_sleep_ticks`,
//! `-EAGAIN` when non-blocking, SIGPIPE to the writer).
//!
//! Registration stays in C: devfs registers `/dev/pipe` and its ioctl calls
//! [`pipe_create`]; `fd_mux.c`'s (dead) `sys_pipe` does too.

use core::ffi::{c_char, c_void};
use core::mem::size_of;
use core::ptr;

use cact_sync::{mutex_t, TaskStruct};

use crate::abi::*;

const PIPE_BUF_SIZE: usize = 4096;
const PIPE_MAGIC: u32 = 0x5049_5045; // "PIPE"
const PIPE_O_NONBLOCK: i32 = 0x0800;
const EAGAIN: i32 = 11;
const EPIPE: i32 = 32;
const SIGPIPE: u32 = 16; // 1 << 4, matches task.h

unsafe extern "C" {
    fn sched_sleep_ticks(ticks: u32);
    fn task_signal(pid: u32, sig: u32);
    fn validate_user_ptr(p: *const c_void, size: u32) -> i32;
    fn cact_current_task_get() -> *mut TaskStruct;
}

struct Pipe {
    magic:      u32,
    buf:        [u8; PIPE_BUF_SIZE],
    read_pos:   u32,
    write_pos:  u32,
    len:        u32,
    flags:      i32,
    write_open: i32,
    read_open:  i32,
    ref_count:  i32, // live vfs_node wrappers
    lock:       mutex_t,
}

#[inline]
unsafe fn pipe_of(node: *mut VfsNode) -> *mut Pipe {
    unsafe { (*node).priv_ as *mut Pipe }
}

#[inline]
unsafe fn node_is_write(node: *mut VfsNode) -> bool {
    unsafe { (*node).inode == 1 }
}

// ── Node ops ────────────────────────────────────────────────────────────

unsafe extern "C" fn pipe_node_read(
    node: *mut VfsNode,
    _off: u32,
    size: u32,
    buf: *mut c_char,
) -> i32 {
    let p = unsafe { pipe_of(node) };
    unsafe { pipe_read(p, size, buf) }
}

unsafe extern "C" fn pipe_node_write(
    node: *mut VfsNode,
    _off: u32,
    size: u32,
    buf: *mut c_char,
) -> i32 {
    let p = unsafe { pipe_of(node) };
    unsafe { pipe_write(p, size, buf) }
}

unsafe extern "C" fn pipe_node_open(node: *mut VfsNode) {
    let p = unsafe { pipe_of(node) };
    if p.is_null() || unsafe { (*p).magic } != PIPE_MAGIC {
        return;
    }
    // SAFETY: `node` is a live pipe node; the mutex guards the counters.
    unsafe {
        (*node).refcount += 1;
        cact_sync::mutex_lock(&mut (*p).lock);
        if node_is_write(node) {
            (*p).write_open += 1;
        } else {
            (*p).read_open += 1;
        }
        cact_sync::mutex_unlock(&mut (*p).lock);
    }
}

unsafe extern "C" fn pipe_node_close(node: *mut VfsNode) {
    let p = unsafe { pipe_of(node) };

    // Already force-destroyed: just release the wrapper.
    if p.is_null() || unsafe { (*p).magic } != PIPE_MAGIC {
        unsafe {
            if (*node).refcount <= 1 {
                cact_mm::kfree(node as *mut u8);
            } else {
                (*node).refcount -= 1;
            }
        }
        return;
    }

    // SAFETY: `p` is a live pipe; counters are under the pipe lock.
    let should_free = unsafe {
        cact_sync::mutex_lock(&mut (*p).lock);
        if node_is_write(node) {
            if (*p).write_open > 0 {
                (*p).write_open -= 1;
            }
        } else if (*p).read_open > 0 {
            (*p).read_open -= 1;
        }
        (*p).ref_count -= 1;
        let f = (*p).ref_count == 0;
        if f {
            (*p).magic = 0;
        }
        cact_sync::mutex_unlock(&mut (*p).lock);
        f
    };

    if should_free {
        unsafe {
            cact_mm::kfree(p as *mut u8);
            (*node).priv_ = ptr::null_mut();
        }
    }

    // SAFETY: release the node wrapper (its `refcount` is its open count).
    unsafe {
        if (*node).refcount <= 1 {
            cact_mm::kfree(node as *mut u8);
        } else {
            (*node).refcount -= 1;
        }
    }
}

unsafe extern "C" fn pipe_node_poll(node: *mut VfsNode, events: u32) -> i32 {
    let p = unsafe { pipe_of(node) };
    if p.is_null() || unsafe { (*p).magic } != PIPE_MAGIC {
        return VFS_POLLERR as i32;
    }
    let is_wr = unsafe { node_is_write(node) };
    // SAFETY: live pipe; readiness read under the pipe lock.
    unsafe {
        let mut revents = 0u32;
        cact_sync::mutex_lock(&mut (*p).lock);
        if is_wr {
            if events & VFS_POLLOUT != 0 && (*p).len < PIPE_BUF_SIZE as u32 {
                revents |= VFS_POLLOUT;
            }
            if (*p).read_open == 0 {
                revents |= VFS_POLLERR;
            }
        } else {
            if events & VFS_POLLIN != 0 && (*p).len > 0 {
                revents |= VFS_POLLIN;
            }
            if (*p).write_open == 0 && (*p).len == 0 {
                revents |= VFS_POLLHUP;
            }
        }
        cact_sync::mutex_unlock(&mut (*p).lock);
        revents as i32
    }
}

static PIPE_OPS: VfsOps = VfsOps {
    read:  Some(pipe_node_read),
    write: Some(pipe_node_write),
    open:  Some(pipe_node_open),
    close: Some(pipe_node_close),
    walk: None,
    readdir: None,
    listdir: None,
    create: None,
    delete: None,
    mkdir: None,
    rmdir: None,
    rename: None,
    symlink: None,
    link: None,
    unlink: None,
    readlink: None,
    ioctl: None,
    mmap_backing: None,
    truncate: None,
    chmod: None,
    chown: None,
    mknod: None,
    stat: None,
    poll: Some(pipe_node_poll),
    lseek: None,
    rename2: None,
};

// ── Core read/write ─────────────────────────────────────────────────────

unsafe fn pipe_read(p: *mut Pipe, size: u32, buffer: *mut c_char) -> i32 {
    if p.is_null() || unsafe { (*p).magic } != PIPE_MAGIC || buffer.is_null() || size == 0 {
        return -1;
    }
    let nonblock = unsafe { (*p).flags } & PIPE_O_NONBLOCK != 0;
    let mut copied: u32 = 0;

    while copied < size {
        // SAFETY: live pipe; state read/copied under the pipe lock.
        unsafe {
            cact_sync::mutex_lock(&mut (*p).lock);

            if (*p).len == 0 {
                if (*p).write_open == 0 {
                    cact_sync::mutex_unlock(&mut (*p).lock);
                    return copied as i32; // EOF
                }
                if nonblock {
                    cact_sync::mutex_unlock(&mut (*p).lock);
                    return if copied > 0 { copied as i32 } else { -EAGAIN };
                }
                cact_sync::mutex_unlock(&mut (*p).lock);
                sched_sleep_ticks(1);
                if validate_user_ptr(buffer.add(copied as usize) as *const c_void, 1) == 0 {
                    return if copied > 0 { copied as i32 } else { -1 };
                }
                continue;
            }

            let want = size - copied;
            let avail = (*p).len;
            let chunk = if want < avail { want } else { avail };
            let to_end = PIPE_BUF_SIZE as u32 - (*p).read_pos;
            if chunk <= to_end {
                ptr::copy_nonoverlapping(
                    (*p).buf.as_ptr().add((*p).read_pos as usize),
                    buffer.add(copied as usize) as *mut u8,
                    chunk as usize,
                );
            } else {
                ptr::copy_nonoverlapping(
                    (*p).buf.as_ptr().add((*p).read_pos as usize),
                    buffer.add(copied as usize) as *mut u8,
                    to_end as usize,
                );
                ptr::copy_nonoverlapping(
                    (*p).buf.as_ptr(),
                    (buffer.add(copied as usize) as *mut u8).add(to_end as usize),
                    (chunk - to_end) as usize,
                );
            }
            (*p).read_pos = ((*p).read_pos + chunk) % PIPE_BUF_SIZE as u32;
            (*p).len -= chunk;
            copied += chunk;

            cact_sync::mutex_unlock(&mut (*p).lock);
        }
    }
    copied as i32
}

unsafe fn pipe_write(p: *mut Pipe, size: u32, buffer: *mut c_char) -> i32 {
    if p.is_null() || unsafe { (*p).magic } != PIPE_MAGIC || buffer.is_null() || size == 0 {
        return -1;
    }

    // Reader already gone → EPIPE before entering the loop.
    // SAFETY: live pipe.
    unsafe {
        cact_sync::mutex_lock(&mut (*p).lock);
        if (*p).read_open == 0 {
            cact_sync::mutex_unlock(&mut (*p).lock);
            let t = cact_current_task_get();
            if !t.is_null() {
                task_signal((*t).pid, SIGPIPE);
            }
            return -EPIPE;
        }
        cact_sync::mutex_unlock(&mut (*p).lock);
    }

    let nonblock = unsafe { (*p).flags } & PIPE_O_NONBLOCK != 0;
    let mut written: u32 = 0;

    while written < size {
        // SAFETY: live pipe; state read/copied under the pipe lock.
        unsafe {
            cact_sync::mutex_lock(&mut (*p).lock);

            if (*p).read_open == 0 {
                cact_sync::mutex_unlock(&mut (*p).lock);
                let t = cact_current_task_get();
                if !t.is_null() {
                    task_signal((*t).pid, SIGPIPE);
                }
                return if written > 0 { written as i32 } else { -EPIPE };
            }

            if (*p).len as usize == PIPE_BUF_SIZE {
                if nonblock {
                    cact_sync::mutex_unlock(&mut (*p).lock);
                    return if written > 0 { written as i32 } else { -EAGAIN };
                }
                cact_sync::mutex_unlock(&mut (*p).lock);
                sched_sleep_ticks(1);
                if validate_user_ptr(buffer.add(written as usize) as *const c_void, 1) == 0 {
                    return if written > 0 { written as i32 } else { -1 };
                }
                continue;
            }

            let want = size - written;
            let space = PIPE_BUF_SIZE as u32 - (*p).len;
            let chunk = if want < space { want } else { space };
            let to_end = PIPE_BUF_SIZE as u32 - (*p).write_pos;
            if chunk <= to_end {
                ptr::copy_nonoverlapping(
                    buffer.add(written as usize) as *const u8,
                    (*p).buf.as_mut_ptr().add((*p).write_pos as usize),
                    chunk as usize,
                );
            } else {
                ptr::copy_nonoverlapping(
                    buffer.add(written as usize) as *const u8,
                    (*p).buf.as_mut_ptr().add((*p).write_pos as usize),
                    to_end as usize,
                );
                ptr::copy_nonoverlapping(
                    (buffer.add(written as usize) as *const u8).add(to_end as usize),
                    (*p).buf.as_mut_ptr(),
                    (chunk - to_end) as usize,
                );
            }
            (*p).write_pos = ((*p).write_pos + chunk) % PIPE_BUF_SIZE as u32;
            (*p).len += chunk;
            written += chunk;

            cact_sync::mutex_unlock(&mut (*p).lock);
        }
    }
    written as i32
}

// ── Node / pipe construction ────────────────────────────────────────────

unsafe fn make_node(p: *mut Pipe, name: *const c_char, is_write: u32) -> *mut VfsNode {
    // SAFETY: kmalloc returns a zeroable block of `size_of::<VfsNode>()` or null.
    let n = cact_mm::kmalloc(size_of::<VfsNode>() as u32) as *mut VfsNode;
    if n.is_null() {
        return ptr::null_mut();
    }
    unsafe {
        ptr::write_bytes(n as *mut u8, 0, size_of::<VfsNode>());

        let mut i = 0usize;
        while *name.add(i) != 0 && i < 127 {
            (*n).name[i] = *name.add(i);
            i += 1;
        }
        (*n).name[i] = 0;

        (*n).type_ = VFS_PIPE;
        (*n).size = PIPE_BUF_SIZE as u32;
        (*n).inode = is_write; // 0 = read end, 1 = write end
        (*n).refcount = 0; // open count; file_alloc's open() bumps it
        (*n).ops = ptr::addr_of!(PIPE_OPS) as *mut VfsOps;
        (*n).priv_ = p as *mut c_void;

        cact_sync::mutex_lock(&mut (*p).lock);
        (*p).ref_count += 1;
        cact_sync::mutex_unlock(&mut (*p).lock);
    }
    n
}

/// Create an anonymous pipe; fills `pipefd` with the read and write nodes.
#[no_mangle]
pub unsafe extern "C" fn pipe_create(pipefd: *mut *mut VfsNode, flags: i32) -> i32 {
    // SAFETY: `pipefd` is a caller-provided two-element array.
    let p = cact_mm::kmalloc(size_of::<Pipe>() as u32) as *mut Pipe;
    if p.is_null() {
        return -1;
    }
    // SAFETY: fresh block; fully initialised before it is published.
    unsafe {
        ptr::write_bytes(p as *mut u8, 0, size_of::<Pipe>());
        (*p).magic = PIPE_MAGIC;
        (*p).flags = flags;
        cact_sync::mutex_init(&mut (*p).lock);

        let r = make_node(p, b"pipe:r\0".as_ptr() as *const c_char, 0);
        if r.is_null() {
            cact_mm::kfree(p as *mut u8);
            return -1;
        }
        let w = make_node(p, b"pipe:w\0".as_ptr() as *const c_char, 1);
        if w.is_null() {
            cact_mm::kfree(r as *mut u8);
            cact_mm::kfree(p as *mut u8);
            return -1;
        }
        *pipefd.add(0) = r;
        *pipefd.add(1) = w;
    }
    0
}

/// Propagate `O_NONBLOCK` into the shared pipe state (called from
/// `sys_fcntl(F_SETFL)`; the original accepted the flag but ignored it).
#[no_mangle]
pub unsafe extern "C" fn vfs_pipe_set_nonblock(node: *mut VfsNode, on: i32) {
    let p = unsafe { pipe_of(node) };
    if p.is_null() || unsafe { (*p).magic } != PIPE_MAGIC {
        return;
    }
    // SAFETY: live pipe; `flags` is a plain word guarded by the pipe lock.
    unsafe {
        cact_sync::mutex_lock(&mut (*p).lock);
        if on != 0 {
            (*p).flags |= PIPE_O_NONBLOCK;
        } else {
            (*p).flags &= !PIPE_O_NONBLOCK;
        }
        cact_sync::mutex_unlock(&mut (*p).lock);
    }
}
