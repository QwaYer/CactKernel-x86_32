//! Syncobj: the fence objects a client waits on and signals.
//!
//! Two flavours, fixed at creation as in Linux: a **binary** object has one
//! signalled bit, a **timeline** object has a point that only moves forward.
//! An exported object becomes an fd backed by a small VFS node that polls
//! readable once the object is signalled — the sync-file shape, so a client can
//! hand the fd to another process (or to `poll()`).
//!
//! `TRANSFER` and `EVENTFD` are not implemented; the ioctls answer `-EINVAL`,
//! and the caps that advertise the rest are honest about what exists.

use alloc::vec;
use alloc::vec::Vec;
use core::ffi::{c_int, c_void};

use crate::ffi::drm_copy_in;
use crate::structs::{DrmDevice, DrmFile, Syncobj};
use crate::vfs::{
    copy_cstr, File, VfsFileOps, VfsNode, VFS_CHARDEVICE, VFS_POLLERR, VFS_POLLIN,
};

/* ── uapi (uapi/drm.h) ──────────────────────────────────────────────────── */

#[repr(C)]
struct SyncobjCreate {
    handle: u32,
    flags: u32,
}

#[repr(C)]
struct SyncobjDestroy {
    handle: u32,
    pad: u32,
}

#[repr(C)]
struct SyncobjHandle {
    handle: u32,
    flags: u32,
    fd: i32,
    pad: u32,
}

#[repr(C)]
struct SyncobjArray {
    handles: u64,
    count_handles: u32,
    pad: u32,
}

#[repr(C)]
struct SyncobjWait {
    handles: u64,
    timeout_nsec: i64,
    count_handles: u32,
    flags: u32,
    first_signaled: u32,
    pad: u32,
    deadline_nsec: u64,
}

#[repr(C)]
struct SyncobjTimelineWait {
    handles: u64,
    points: u64,
    timeout_nsec: i64,
    count_handles: u32,
    flags: u32,
    first_signaled: u32,
    pad: u32,
    deadline_nsec: u64,
}

#[repr(C)]
struct SyncobjTimelineArray {
    handles: u64,
    points: u64,
    count_handles: u32,
    flags: u32,
}

const _: () = assert!(core::mem::size_of::<SyncobjCreate>() == 8);
const _: () = assert!(core::mem::size_of::<SyncobjDestroy>() == 8);
const _: () = assert!(core::mem::size_of::<SyncobjHandle>() == 16);
const _: () = assert!(core::mem::offset_of!(SyncobjHandle, fd) == 8);
const _: () = assert!(core::mem::size_of::<SyncobjArray>() == 16);
const _: () = assert!(core::mem::size_of::<SyncobjWait>() == 40);
const _: () = assert!(core::mem::offset_of!(SyncobjWait, first_signaled) == 24);
const _: () = assert!(core::mem::offset_of!(SyncobjWait, deadline_nsec) == 32);
const _: () = assert!(core::mem::size_of::<SyncobjTimelineWait>() == 48);
const _: () = assert!(core::mem::size_of::<SyncobjTimelineArray>() == 24);
const _: () = assert!(core::mem::offset_of!(SyncobjTimelineArray, points) == 8);

const fn drm_iowr(nr: u32, size: u32) -> u32 {
    (3u32 << 30) | (0x64u32 << 8) | nr | (size << 16)
}
const DRM_IOCTL_SYNCOBJ_CREATE: u32 = drm_iowr(0xBF, 8);
const DRM_IOCTL_SYNCOBJ_DESTROY: u32 = drm_iowr(0xC0, 8);
const DRM_IOCTL_SYNCOBJ_HANDLE_TO_FD: u32 = drm_iowr(0xC1, 16);
const DRM_IOCTL_SYNCOBJ_FD_TO_HANDLE: u32 = drm_iowr(0xC2, 16);
const DRM_IOCTL_SYNCOBJ_WAIT: u32 = drm_iowr(0xC3, 40);
const DRM_IOCTL_SYNCOBJ_RESET: u32 = drm_iowr(0xC4, 16);
const DRM_IOCTL_SYNCOBJ_SIGNAL: u32 = drm_iowr(0xC5, 16);
const DRM_IOCTL_SYNCOBJ_TIMELINE_WAIT: u32 = drm_iowr(0xCA, 48);
const DRM_IOCTL_SYNCOBJ_QUERY: u32 = drm_iowr(0xCB, 24);
const DRM_IOCTL_SYNCOBJ_TIMELINE_SIGNAL: u32 = drm_iowr(0xCD, 24);

const DRM_SYNCOBJ_CREATE_SIGNALED: u32 = 1 << 0;
/// The ABI value for a timeline object; the vendored `uapi/drm.h` predates the
/// macro, but libdrm sends it.
const DRM_SYNCOBJ_CREATE_TYPE_TIMELINE: u32 = 1 << 1;
const DRM_SYNCOBJ_HANDLE_TO_FD_FLAGS_EXPORT_SYNC_FILE: u32 = 1 << 0;
const DRM_SYNCOBJ_FD_TO_HANDLE_FLAGS_IMPORT_SYNC_FILE: u32 = 1 << 0;
const DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL: u32 = 1 << 0;

/* A wait is bounded: 100 Hz ticks, so 30 is about 300 ms. */
const SYNC_WAIT_MAX_TICKS: u32 = 30;
const MAX_SYNCOBJ_HANDLES: usize = 64;

extern "C" {
    fn kmalloc(size: u32) -> *mut c_void;
    fn kfree(ptr: *mut c_void);
    fn alloc_fd(node: *mut c_void) -> c_int;
    fn sched_sleep_ticks(ticks: u32);
    fn validate_user_ptr(ptr: *const c_void, size: u32) -> c_int;
    fn copy_from_user(dst: *mut c_void, src: *const c_void, size: u32) -> c_int;
}

/* ── what an exported fd's node points at ───────────────────────────────── */

#[repr(C)]
struct SyncFileRef {
    dev: *mut DrmDevice,
    id: u32,
}

fn syncobj_signaled(dev: *mut DrmDevice, id: u32) -> bool {
    // SAFETY: caller's device.
    unsafe {
        let o = (*dev).find_syncobj(id);
        !o.is_null() && (*o).signaled
    }
}

extern "C" fn sync_file_poll(node: *mut VfsNode, _priv_: *mut c_void, _events: u32) -> c_int {
    // SAFETY: devfs passes one of our own nodes.
    unsafe {
        let r = (*node).priv_ as *mut SyncFileRef;
        if r.is_null() {
            return VFS_POLLERR as c_int;
        }
        if syncobj_signaled((*r).dev, (*r).id) {
            VFS_POLLIN as c_int
        } else {
            0
        }
    }
}

/// A sync file is waited on, not read — as on Linux.
extern "C" fn sync_file_read(
    _node: *mut VfsNode,
    _priv_: *mut c_void,
    _off: u32,
    _size: u32,
    _buf: *mut u8,
) -> c_int {
    -22
}

extern "C" fn sync_file_ioctl(
    _node: *mut VfsNode,
    _priv_: *mut c_void,
    _cmd: u32,
    _arg: *mut c_void,
) -> c_int {
    -22
}

/// Closing the fd drops the export record, so the fd can no longer be imported.
extern "C" fn sync_file_release(node: *mut VfsNode, _f: *mut File) {
    // SAFETY: devfs passes one of our own nodes, whose priv we allocated.
    unsafe {
        let r = (*node).priv_ as *mut SyncFileRef;
        if r.is_null() {
            return;
        }
        let dev = (*r).dev;
        if !dev.is_null() {
            (*dev).sync_fds.retain(|&(_, id, n)| !(n == node && id == (*r).id));
        }
        kfree(r as *mut c_void);
        (*node).priv_ = core::ptr::null_mut();
    }
}

static mut SYNC_FILE_FOPS: VfsFileOps = VfsFileOps {
    read: Some(sync_file_read),
    write: None,
    ioctl: Some(sync_file_ioctl),
    poll: Some(sync_file_poll),
    open: None,
    release: Some(sync_file_release),
};

/* ── per-file handles ───────────────────────────────────────────────────── */

unsafe fn handle_of(file: *mut DrmFile, id: u32) -> u32 {
    (*file).next_syncobj_handle += 1;
    let h = (*file).next_syncobj_handle;
    (*file).syncobjs.insert(h, id);
    h
}

unsafe fn object_of(file: *mut DrmFile, handle: u32) -> *mut Syncobj {
    if file.is_null() || handle == 0 {
        return core::ptr::null_mut();
    }
    match (*file).syncobjs.get(&handle) {
        Some(&id) => (*(*file).dev).find_syncobj(id),
        None => core::ptr::null_mut(),
    }
}

unsafe fn read_u32s(ptr: u64, out: &mut [u32]) -> bool {
    if out.is_empty() {
        return true;
    }
    let user = ptr as u32 as usize as *const c_void;
    if user.is_null() {
        return false;
    }
    let bytes = (out.len() * 4) as u32;
    validate_user_ptr(user, bytes) != 0 && copy_from_user(out.as_mut_ptr() as *mut c_void, user, bytes) == 0
}

unsafe fn read_u64s(ptr: u64, out: &mut [u64]) -> bool {
    if out.is_empty() {
        return true;
    }
    let user = ptr as u32 as usize as *const c_void;
    if user.is_null() {
        return false;
    }
    let bytes = (out.len() * 8) as u32;
    validate_user_ptr(user, bytes) != 0
        && copy_from_user(out.as_mut_ptr() as *mut c_void, user, bytes) == 0
}

unsafe fn write_u64s(ptr: u64, src: &[u64]) -> bool {
    if src.is_empty() {
        return true;
    }
    let user = ptr as u32 as usize as *mut c_void;
    if user.is_null() {
        return false;
    }
    crate::ffi::drm_put_raw(user, src.as_ptr() as *const c_void, (src.len() * 8) as u32) == 0
}

/// How many 10 ms ticks a wait may spin for.
fn wait_ticks(timeout_nsec: i64) -> u32 {
    if timeout_nsec <= 0 {
        return SYNC_WAIT_MAX_TICKS; // "no timeout": still bounded, never endless
    }
    let t = (timeout_nsec / 10_000_000) as u32 + 1;
    if t > SYNC_WAIT_MAX_TICKS {
        SYNC_WAIT_MAX_TICKS
    } else {
        t
    }
}

/* ── ioctls ─────────────────────────────────────────────────────────────── */

#[no_mangle]
pub extern "C" fn syncobj_ioctl_create(file: *mut DrmFile, arg: *mut c_void) -> c_int {
    if file.is_null() {
        return -22;
    }
    let mut req = SyncobjCreate { handle: 0, flags: 0 };
    if drm_copy_in(&mut req as *mut _ as *mut c_void, DRM_IOCTL_SYNCOBJ_CREATE, arg, 8) != 0 {
        return -22;
    }
    if req.flags & !(DRM_SYNCOBJ_CREATE_SIGNALED | DRM_SYNCOBJ_CREATE_TYPE_TIMELINE) != 0 {
        return -22;
    }
    // SAFETY: caller's file.
    unsafe {
        let dev = (*file).dev;
        if dev.is_null() {
            return -22;
        }
        let timeline = req.flags & DRM_SYNCOBJ_CREATE_TYPE_TIMELINE != 0;
        let signaled = !timeline && req.flags & DRM_SYNCOBJ_CREATE_SIGNALED != 0;
        let id = (*dev).syncobj_create(timeline, signaled, 0);
        req.handle = handle_of(file, id);
    }
    crate::ffi::drm_copy_out(arg, &req as *const _ as *const c_void, 8)
}

#[no_mangle]
pub extern "C" fn syncobj_ioctl_destroy(file: *mut DrmFile, arg: *mut c_void) -> c_int {
    if file.is_null() {
        return -22;
    }
    let mut req = SyncobjDestroy { handle: 0, pad: 0 };
    if drm_copy_in(&mut req as *mut _ as *mut c_void, DRM_IOCTL_SYNCOBJ_DESTROY, arg, 8) != 0 {
        return -22;
    }
    // SAFETY: caller's file.
    unsafe {
        let id = match (*file).syncobjs.remove(&req.handle) {
            Some(id) => id,
            None => return -2, // -ENOENT
        };
        // The object goes away when the last handle to it does.
        let still_held = (*file).syncobjs.values().any(|&v| v == id);
        if !still_held {
            let dev = (*file).dev;
            if !dev.is_null() {
                (*dev).syncobj_destroy(id);
            }
        }
    }
    0
}

#[no_mangle]
pub extern "C" fn syncobj_ioctl_handle_to_fd(file: *mut DrmFile, arg: *mut c_void) -> c_int {
    if file.is_null() {
        return -22;
    }
    let mut req = SyncobjHandle {
        handle: 0,
        flags: 0,
        fd: -1,
        pad: 0,
    };
    if drm_copy_in(&mut req as *mut _ as *mut c_void, DRM_IOCTL_SYNCOBJ_HANDLE_TO_FD, arg, 16) != 0
    {
        return -22;
    }
    if req.flags & !DRM_SYNCOBJ_HANDLE_TO_FD_FLAGS_EXPORT_SYNC_FILE != 0 {
        return -22;
    }
    // SAFETY: caller's file.
    unsafe {
        let dev = (*file).dev;
        if dev.is_null() {
            return -22;
        }
        let obj = object_of(file, req.handle);
        if obj.is_null() {
            return -2;
        }
        let id = (*obj).id;

        // Exporting the same object twice hands back the same fd, as on Linux.
        for &(fd, sid, _) in (*dev).sync_fds.iter() {
            if sid == id {
                req.fd = fd;
                return crate::ffi::drm_copy_out(arg, &req as *const _ as *const c_void, 16);
            }
        }

        let node = kmalloc(core::mem::size_of::<VfsNode>() as u32) as *mut VfsNode;
        if node.is_null() {
            return -12;
        }
        core::ptr::write_bytes(node, 0, 1);
        copy_cstr((*node).name.as_mut_ptr(), 128, b"syncobj\0".as_ptr());
        (*node).ntype = VFS_CHARDEVICE;
        (*node).fops = core::ptr::addr_of_mut!(SYNC_FILE_FOPS);

        let r = kmalloc(core::mem::size_of::<SyncFileRef>() as u32) as *mut SyncFileRef;
        if r.is_null() {
            kfree(node as *mut c_void);
            return -12;
        }
        (*r).dev = dev;
        (*r).id = id;
        (*node).priv_ = r as *mut c_void;

        let fd = alloc_fd(node as *mut c_void);
        if fd < 0 {
            kfree(r as *mut c_void);
            kfree(node as *mut c_void);
            return -24; // -EMFILE
        }
        (*dev).sync_fds.push((fd, id, node));
        req.fd = fd;
    }
    crate::ffi::drm_copy_out(arg, &req as *const _ as *const c_void, 16)
}

#[no_mangle]
pub extern "C" fn syncobj_ioctl_fd_to_handle(file: *mut DrmFile, arg: *mut c_void) -> c_int {
    if file.is_null() {
        return -22;
    }
    let mut req = SyncobjHandle {
        handle: 0,
        flags: 0,
        fd: -1,
        pad: 0,
    };
    if drm_copy_in(&mut req as *mut _ as *mut c_void, DRM_IOCTL_SYNCOBJ_FD_TO_HANDLE, arg, 16) != 0
    {
        return -22;
    }
    if req.flags & !DRM_SYNCOBJ_FD_TO_HANDLE_FLAGS_IMPORT_SYNC_FILE != 0 {
        return -22;
    }
    // SAFETY: caller's file.
    unsafe {
        let dev = (*file).dev;
        if dev.is_null() {
            return -22;
        }
        let mut id = 0u32;
        for &(fd, sid, _) in (*dev).sync_fds.iter() {
            if fd == req.fd {
                id = sid;
                break;
            }
        }
        if id == 0 {
            return -22; // not a sync file this device exported
        }
        req.handle = handle_of(file, id);
    }
    crate::ffi::drm_copy_out(arg, &req as *const _ as *const c_void, 16)
}

#[no_mangle]
pub extern "C" fn syncobj_ioctl_signal(file: *mut DrmFile, arg: *mut c_void) -> c_int {
    syncobj_set(file, arg, DRM_IOCTL_SYNCOBJ_SIGNAL, true)
}

#[no_mangle]
pub extern "C" fn syncobj_ioctl_reset(file: *mut DrmFile, arg: *mut c_void) -> c_int {
    syncobj_set(file, arg, DRM_IOCTL_SYNCOBJ_RESET, false)
}

/// SIGNAL and RESET differ only in the value they store.
fn syncobj_set(file: *mut DrmFile, arg: *mut c_void, cmd: u32, on: bool) -> c_int {
    if file.is_null() {
        return -22;
    }
    let mut req = SyncobjArray {
        handles: 0,
        count_handles: 0,
        pad: 0,
    };
    if drm_copy_in(&mut req as *mut _ as *mut c_void, cmd, arg, 16) != 0 {
        return -22;
    }
    let n = req.count_handles as usize;
    if n == 0 || n > MAX_SYNCOBJ_HANDLES {
        return -22;
    }
    // SAFETY: caller's file; the handle array is range-checked before use.
    unsafe {
        let mut handles: Vec<u32> = vec![0u32; n];
        if !read_u32s(req.handles, &mut handles) {
            return -14;
        }
        for h in handles {
            let o = object_of(file, h);
            if o.is_null() {
                return -2;
            }
            if (*o).timeline {
                return -22; // use the timeline ioctls for a timeline object
            }
            (*o).signaled = on;
        }
    }
    0
}

#[no_mangle]
pub extern "C" fn syncobj_ioctl_wait(file: *mut DrmFile, arg: *mut c_void) -> c_int {
    if file.is_null() {
        return -22;
    }
    let mut req = SyncobjWait {
        handles: 0,
        timeout_nsec: 0,
        count_handles: 0,
        flags: 0,
        first_signaled: 0,
        pad: 0,
        deadline_nsec: 0,
    };
    if drm_copy_in(&mut req as *mut _ as *mut c_void, DRM_IOCTL_SYNCOBJ_WAIT, arg, 40) != 0 {
        return -22;
    }
    let n = req.count_handles as usize;
    if n == 0 || n > MAX_SYNCOBJ_HANDLES {
        return -22;
    }
    let all = req.flags & DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL != 0;

    // SAFETY: caller's file; the handle array is range-checked before use.
    unsafe {
        let mut handles: Vec<u32> = vec![0u32; n];
        if !read_u32s(req.handles, &mut handles) {
            return -14;
        }
        let mut objs: Vec<*mut Syncobj> = Vec::with_capacity(n);
        for &h in handles.iter() {
            let o = object_of(file, h);
            if o.is_null() {
                return -2;
            }
            if (*o).timeline {
                return -22; // timeline objects use TIMELINE_WAIT
            }
            objs.push(o);
        }

        let mut ticks = wait_ticks(req.timeout_nsec);
        loop {
            let mut done = 0usize;
            let mut first = 0u32;
            for (i, &o) in objs.iter().enumerate() {
                if (*o).signaled {
                    done += 1;
                    if first == 0 {
                        first = i as u32;
                    }
                }
            }
            let satisfied = if all { done == n } else { done > 0 };
            if satisfied {
                req.first_signaled = first;
                break;
            }
            if ticks == 0 {
                return -62; // -ETIME
            }
            ticks -= 1;
            sched_sleep_ticks(1);
        }
    }
    crate::ffi::drm_copy_out(arg, &req as *const _ as *const c_void, 40)
}

#[no_mangle]
pub extern "C" fn syncobj_ioctl_timeline_signal(file: *mut DrmFile, arg: *mut c_void) -> c_int {
    if file.is_null() {
        return -22;
    }
    let mut req = SyncobjTimelineArray {
        handles: 0,
        points: 0,
        count_handles: 0,
        flags: 0,
    };
    if drm_copy_in(&mut req as *mut _ as *mut c_void, DRM_IOCTL_SYNCOBJ_TIMELINE_SIGNAL, arg, 24) != 0
    {
        return -22;
    }
    let n = req.count_handles as usize;
    if n == 0 || n > MAX_SYNCOBJ_HANDLES {
        return -22;
    }
    // SAFETY: caller's file; both arrays are range-checked before use.
    unsafe {
        let mut handles: Vec<u32> = vec![0u32; n];
        let mut points: Vec<u64> = vec![0u64; n];
        if !read_u32s(req.handles, &mut handles) || !read_u64s(req.points, &mut points)
        {
            return -14;
        }
        for i in 0..n {
            let o = object_of(file, handles[i]);
            if o.is_null() {
                return -2;
            }
            if !(*o).timeline {
                return -22;
            }
            // A timeline point only moves forward.
            if points[i] > (*o).point {
                (*o).point = points[i];
            }
        }
    }
    0
}

#[no_mangle]
pub extern "C" fn syncobj_ioctl_query(file: *mut DrmFile, arg: *mut c_void) -> c_int {
    if file.is_null() {
        return -22;
    }
    let mut req = SyncobjTimelineArray {
        handles: 0,
        points: 0,
        count_handles: 0,
        flags: 0,
    };
    if drm_copy_in(&mut req as *mut _ as *mut c_void, DRM_IOCTL_SYNCOBJ_QUERY, arg, 24) != 0 {
        return -22;
    }
    let n = req.count_handles as usize;
    if n == 0 || n > MAX_SYNCOBJ_HANDLES {
        return -22;
    }
    // SAFETY: caller's file; both arrays are range-checked before use.
    unsafe {
        let mut handles: Vec<u32> = vec![0u32; n];
        if !read_u32s(req.handles, &mut handles) {
            return -14;
        }
        let mut points: Vec<u64> = vec![0u64; n];
        for i in 0..n {
            let o = object_of(file, handles[i]);
            if o.is_null() {
                return -2;
            }
            if !(*o).timeline {
                return -22;
            }
            points[i] = (*o).point;
        }
        if !write_u64s(req.points, &points) {
            return -14;
        }
    }
    0
}

#[no_mangle]
pub extern "C" fn syncobj_ioctl_timeline_wait(file: *mut DrmFile, arg: *mut c_void) -> c_int {
    if file.is_null() {
        return -22;
    }
    let mut req = SyncobjTimelineWait {
        handles: 0,
        points: 0,
        timeout_nsec: 0,
        count_handles: 0,
        flags: 0,
        first_signaled: 0,
        pad: 0,
        deadline_nsec: 0,
    };
    if drm_copy_in(&mut req as *mut _ as *mut c_void, DRM_IOCTL_SYNCOBJ_TIMELINE_WAIT, arg, 48) != 0
    {
        return -22;
    }
    let n = req.count_handles as usize;
    if n == 0 || n > MAX_SYNCOBJ_HANDLES {
        return -22;
    }
    let all = req.flags & DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL != 0;

    // SAFETY: caller's file; both arrays are range-checked before use.
    unsafe {
        let mut handles: Vec<u32> = vec![0u32; n];
        let mut points: Vec<u64> = vec![0u64; n];
        if !read_u32s(req.handles, &mut handles) || !read_u64s(req.points, &mut points)
        {
            return -14;
        }
        let mut objs: Vec<*mut Syncobj> = Vec::with_capacity(n);
        for &h in handles.iter() {
            let o = object_of(file, h);
            if o.is_null() {
                return -2;
            }
            if !(*o).timeline {
                return -22;
            }
            objs.push(o);
        }

        let mut ticks = wait_ticks(req.timeout_nsec);
        loop {
            let mut done = 0usize;
            let mut first = 0u32;
            for (i, &o) in objs.iter().enumerate() {
                if (*o).point >= points[i] {
                    done += 1;
                    if first == 0 {
                        first = i as u32;
                    }
                }
            }
            let satisfied = if all { done == n } else { done > 0 };
            if satisfied {
                req.first_signaled = first;
                break;
            }
            if ticks == 0 {
                return -62; // -ETIME
            }
            ticks -= 1;
            sched_sleep_ticks(1);
        }
    }
    crate::ffi::drm_copy_out(arg, &req as *const _ as *const c_void, 48)
}
