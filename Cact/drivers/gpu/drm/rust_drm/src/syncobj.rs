//! Syncobj: the fence objects a client waits on and signals.
//!
//! Two flavours, fixed at creation as in Linux: a **binary** object has one
//! signalled bit, a **timeline** object has a point that only moves forward.
//! An exported object becomes an fd backed by a small VFS node that polls
//! readable once the object is signalled — the sync-file shape, so a client can
//! hand the fd to another process (or to `poll()`).
//!
//! This kernel has no fence objects, so an object's *state* is all a fence would
//! carry: signalled/not for a binary object, the point reached for a timeline
//! one.  Everything fence-shaped is expressed in those terms:
//!
//!   * `TRANSFER` moves that state between objects (a signalled source into a
//!     signalled binary destination, or a point the destination has reached);
//!   * `EVENTFD` registers an eventfd to be incremented once, when the object
//!     reaches a point — the registration holds the eventfd open, so it works
//!     even if the registering process closed the fd (or died) meanwhile;
//!   * `WAIT_FOR_SUBMIT` on a transfer waits for the source to reach that state,
//!     which here is the same instant the point becomes signalled.  Nothing in
//!     the kernel produces points on its own, so the wait is the bounded poll
//!     the `WAIT` ioctls already use, not an interruptible fence wait.
//!
//! The remaining inexpressible corner is a point that is *submitted but
//! pending*: `WAIT_AVAILABLE` and its absence are therefore indistinguishable
//! here, and an object never holds a fence it has not reached.

use alloc::vec;
use alloc::vec::Vec;
use core::ffi::{c_int, c_void};

use crate::ffi::drm_copy_in;
use crate::gem::lock_ptr;
use crate::structs::{DrmDevice, DrmFile, IrqSpinlock, Syncobj, SyncobjEventfdEntry};
use crate::syncobj_state::SyncState;
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

#[repr(C)]
struct SyncobjTransfer {
    src_handle: u32,
    dst_handle: u32,
    src_point: u64,
    dst_point: u64,
    flags: u32,
    pad: u32,
}

#[repr(C)]
struct SyncobjEventfd {
    handle: u32,
    flags: u32,
    point: u64,
    fd: i32,
    pad: u32,
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
const _: () = assert!(core::mem::size_of::<SyncobjTransfer>() == 32);
const _: () = assert!(core::mem::offset_of!(SyncobjTransfer, src_point) == 8);
const _: () = assert!(core::mem::offset_of!(SyncobjTransfer, flags) == 24);
const _: () = assert!(core::mem::size_of::<SyncobjEventfd>() == 24);
const _: () = assert!(core::mem::offset_of!(SyncobjEventfd, point) == 8);
const _: () = assert!(core::mem::offset_of!(SyncobjEventfd, fd) == 16);

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
const DRM_IOCTL_SYNCOBJ_TRANSFER: u32 = drm_iowr(0xCC, 32);
const DRM_IOCTL_SYNCOBJ_TIMELINE_SIGNAL: u32 = drm_iowr(0xCD, 24);
const DRM_IOCTL_SYNCOBJ_EVENTFD: u32 = drm_iowr(0xCF, 24);

const DRM_SYNCOBJ_CREATE_SIGNALED: u32 = 1 << 0;
/// The ABI value for a timeline object; the vendored `uapi/drm.h` predates the
/// macro, but libdrm sends it.
const DRM_SYNCOBJ_CREATE_TYPE_TIMELINE: u32 = 1 << 1;
const DRM_SYNCOBJ_HANDLE_TO_FD_FLAGS_EXPORT_SYNC_FILE: u32 = 1 << 0;
const DRM_SYNCOBJ_FD_TO_HANDLE_FLAGS_IMPORT_SYNC_FILE: u32 = 1 << 0;
const DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL: u32 = 1 << 0;
const DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT: u32 = 1 << 1;
const DRM_SYNCOBJ_WAIT_FLAGS_WAIT_AVAILABLE: u32 = 1 << 2;

/* A wait is bounded: 100 Hz ticks, so 30 is about 300 ms. */
const SYNC_WAIT_MAX_TICKS: u32 = 30;
const MAX_SYNCOBJ_HANDLES: usize = 64;

extern "C" {

    fn alloc_fd(node: *mut c_void) -> c_int;
    fn sched_sleep_ticks(ticks: u32);
    fn validate_user_ptr(ptr: *const c_void, size: u32) -> c_int;
    fn copy_from_user(dst: *mut c_void, src: *const c_void, size: u32) -> c_int;

    /* fs/eventfd/eventfd.c — the eventfd an EVENTFD registration holds. */
    fn eventfd_node_get(fd: c_int, out: *mut *mut VfsNode) -> c_int;
    fn eventfd_node_put(node: *mut VfsNode);
    fn eventfd_node_signal(node: *mut VfsNode, n: u64) -> c_int;

    /* kernel/sync — the device bookkeeping lock, also used by the GEM paths. */
    fn irq_spinlock_acquire(lock: *mut IrqSpinlock);
    fn irq_spinlock_release(lock: *mut IrqSpinlock);
}

/* ── what an exported fd's node points at ───────────────────────────────── */

#[repr(C)]
struct SyncFileRef {
    dev: *mut DrmDevice,
    id: u32,
}

/// Is the object the sync-file fd stands for done?  A binary object is done when
/// its bit is set, a timeline object when it has reached a point at all — which
/// is what `reached(0)` asks for, so the exported fd becomes readable for a
/// timeline object too (it used to poll the binary bit, which a timeline object
/// never sets).
fn syncobj_signaled(dev: *mut DrmDevice, id: u32) -> bool {
    // SAFETY: `dev` is the caller's live device; this finds the object by id.
    let o = unsafe { (*dev).find_syncobj(id) };
    if o.is_null() {
        return false;
    }
    // SAFETY: `o` is the live object named by `id`; this reads its state.
    unsafe { state_of(o) }.reached(0)
}

extern "C" fn sync_file_poll(node: *mut VfsNode, _priv_: *mut c_void, _events: u32) -> c_int {
    // SAFETY: devfs passes one of our own nodes, so this `priv_` read is in bounds.
    let r = unsafe { (*node).priv_ } as *mut SyncFileRef;
    if r.is_null() {
        return VFS_POLLERR as c_int;
    }
    // SAFETY: `r` is the live export record this node owns; this reads its device.
    let dev = unsafe { (*r).dev };
    // SAFETY: as above — the syncobj id it names.
    let id = unsafe { (*r).id };
    if syncobj_signaled(dev, id) {
        VFS_POLLIN as c_int
    } else {
        0
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
    // SAFETY: devfs passes one of our own nodes, so this `priv_` read is in bounds.
    let r = unsafe { (*node).priv_ } as *mut SyncFileRef;
    if r.is_null() {
        return;
    }
    // SAFETY: `r` is the live export record this node owns; this reads its device.
    let dev = unsafe { (*r).dev };
    // SAFETY: as above — the syncobj id it names.
    let id = unsafe { (*r).id };
    if !dev.is_null() {
        // SAFETY: `dev` is the live device; this drops the export record for `node`.
        unsafe { (*dev).sync_fds.retain(|&(_, sid, n)| !(n == node && sid == id)) };
    }
    // SAFETY: `r` was allocated by `kmalloc` and is owned by this node.
    unsafe { cact_mm::kfree(r as *mut u8) };
    // SAFETY: clearing the node's private pointer after the free.
    unsafe { (*node).priv_ = core::ptr::null_mut() };
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
    // SAFETY: `file` is the caller's open client; `next_syncobj_handle` only moves
    // forward, so the handle minted here is never a live key already, and this
    // borrow is exclusive for the mint.
    let file = unsafe { &mut *file };
    file.next_syncobj_handle += 1;
    let h = file.next_syncobj_handle;
    file.syncobjs.insert(h, id);
    h
}

unsafe fn object_of(file: *mut DrmFile, handle: u32) -> *mut Syncobj {
    if file.is_null() || handle == 0 {
        return core::ptr::null_mut();
    }
    // SAFETY: `file` is non-null (checked above) and belongs to the caller, so this
    // handle-map lookup is in bounds.
    let id = match unsafe { (*file).syncobjs.get(&handle) } {
        Some(&id) => id,
        None => return core::ptr::null_mut(),
    };
    // SAFETY: `file` is the live client; this reads the device that minted the id.
    let dev = unsafe { (*file).dev };
    // SAFETY: `dev` is that live device and it created the id, so `find_syncobj`
    // only returns a live object or null.
    unsafe { (*dev).find_syncobj(id) }
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
    // SAFETY: `user` is a non-null userspace pointer and `bytes` is exactly the size
    // of `out`; this range-checks it.
    if unsafe { validate_user_ptr(user, bytes) } == 0 {
        return false;
    }
    // SAFETY: the range check above passed, so `user` is a readable userspace range
    // of `bytes` bytes and `out` is a writable buffer of that size.
    unsafe { copy_from_user(out.as_mut_ptr() as *mut c_void, user, bytes) == 0 }
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
    // SAFETY: `user` is a non-null userspace pointer and `bytes` is exactly the size
    // of `out`; this range-checks it.
    if unsafe { validate_user_ptr(user, bytes) } == 0 {
        return false;
    }
    // SAFETY: the range check above passed, so `user` is a readable userspace range
    // of `bytes` bytes and `out` is a writable buffer of that size.
    unsafe { copy_from_user(out.as_mut_ptr() as *mut c_void, user, bytes) == 0 }
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

/* ── fence state, in this kernel's terms ────────────────────────────────── */

/// Read an object's observable state.  The rules over it live in
/// [`crate::syncobj_state`], away from the device state.
///
/// SAFETY: `o` is a live syncobj.
unsafe fn state_of(o: *mut Syncobj) -> SyncState {
    // SAFETY: the caller's contract is `o` being live; one field read per block.
    let timeline = unsafe { (*o).timeline };
    // SAFETY: as above.
    let signaled = unsafe { (*o).signaled };
    // SAFETY: as above.
    let point = unsafe { (*o).point };
    SyncState {
        timeline,
        signaled,
        point,
    }
}

/// Write back the half of the state that moves.  The flavour is fixed at
/// creation, so it is never written back.
///
/// SAFETY: `o` is a live syncobj and `st` describes it (a state read from it, or
/// one whose [`SyncState::advance`]/[`SyncState::take_transfer`] has run).
unsafe fn set_state(o: *mut Syncobj, st: SyncState) {
    // SAFETY: the caller's contract is `o` being live; one field write per block.
    unsafe { (*o).signaled = st.signaled };
    // SAFETY: as above.
    unsafe { (*o).point = st.point };
}

/// `drm_syncobj_find_fence()` in this kernel's terms: 0 when the state `handle`
/// carries at `point` is there, negative errno otherwise — `-ENOENT` for an
/// unknown handle, `-EINVAL` while the state is not there (Linux: a NULL fence)
/// or for a point on an object that has none, `-ETIME` for a `WAIT_FOR_SUBMIT`
/// wait that ran out.
///
/// Only `WAIT_FOR_SUBMIT` may be asked for, as on Linux.  Nothing in the kernel
/// produces points by itself, so waiting for one is the bounded poll the `WAIT`
/// ioctls use rather than an interruptible fence wait.
///
/// SAFETY: `file` is an open client.
unsafe fn find_state(file: *mut DrmFile, handle: u32, point: u64, flags: u32) -> c_int {
    if flags & !DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT != 0 {
        return -22;
    }
    // SAFETY: the caller's contract is `file` being an open client.
    let o = unsafe { object_of(file, handle) };
    if o.is_null() {
        return -2; // -ENOENT
    }
    // A binary object has no points, so a non-zero one is a bad argument — as on
    // Linux, where the sequence lookup on a non-chain fence fails.
    // SAFETY: `o` is the live object resolved above.
    if !unsafe { state_of(o) }.takes_point(point) {
        return -22;
    }
    let wait = flags & DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT != 0;
    let mut ticks = wait_ticks(0);
    loop {
        // SAFETY: `o` is the live object resolved above; the state is re-read
        // because another client can move it while this one waits.
        if unsafe { state_of(o) }.reached(point) {
            return 0;
        }
        if !wait {
            return -22;
        }
        if ticks == 0 {
            return -62; // -ETIME
        }
        ticks -= 1;
        // SAFETY: `sched_sleep_ticks` suspends the calling task for one tick.
        unsafe { sched_sleep_ticks(1) };
    }
}

/* ── eventfd registrations ──────────────────────────────────────────────── */

/// Add a registration under the device lock, so it cannot be lost against a
/// concurrent reap (which takes the list out from under the same lock).
///
/// SAFETY: `dev` is a live device.
unsafe fn eventfd_register(dev: *mut DrmDevice, entry: SyncobjEventfdEntry) {
    // SAFETY: `dev` is live; the lock word belongs to it.
    let lock = unsafe { lock_ptr(dev) };
    // SAFETY: `lock` is the device's lock; nothing is called out to under it.
    unsafe { irq_spinlock_acquire(lock) };
    // SAFETY: `dev` is live; the registration list is the device's own.
    unsafe { (*dev).syncobj_ev_fds.push(entry) };
    // SAFETY: `lock` is the device's lock, held since the acquire above.
    unsafe { irq_spinlock_release(lock) };
}

/// Release every pending registration of a device that is being torn down,
/// without signalling any (nothing can be signalled then).
///
/// SAFETY: `dev` is a live device in the middle of `drm_dev_unregister`.
pub(crate) unsafe fn eventfd_drop_all(dev: *mut DrmDevice) {
    if dev.is_null() {
        return;
    }
    // SAFETY: `dev` is live; the lock word belongs to it.
    let lock = unsafe { lock_ptr(dev) };
    // SAFETY: `lock` is the device's lock and nothing else holds it here; the
    // eventfds are released after the release below.
    unsafe { irq_spinlock_acquire(lock) };
    // SAFETY: `dev` is live; taking the list leaves an empty one behind.
    let entries = core::mem::take(unsafe { &mut (*dev).syncobj_ev_fds });
    // SAFETY: `lock` is the device's lock, held since the acquire above.
    unsafe { irq_spinlock_release(lock) };

    for e in entries {
        // SAFETY: `e.node` is the reference the registration held, and it is not
        // used again.
        unsafe { eventfd_node_put(e.node) };
    }
}

/// Reap the eventfd registrations waiting on `id`: the ones whose object has now
/// reached their point are incremented once, and with `notify` false (the object
/// is going away) the ones still waiting are released without being signalled —
/// as Linux frees the entries of a syncobj it is freeing.  Registrations on
/// other objects are left alone.
///
/// The list is walked under the device lock so two signalers cannot both take
/// the same entry, and the eventfd is incremented outside it, because that path
/// takes a mutex.
///
/// SAFETY: `dev` is a live device.
unsafe fn eventfd_reap(dev: *mut DrmDevice, id: u32, notify: bool) {
    if dev.is_null() {
        return;
    }
    // SAFETY: `dev` is live; the lock word belongs to it.
    let lock = unsafe { lock_ptr(dev) };
    // SAFETY: `lock` is the device's lock and nothing else holds it here; the
    // mutex that `eventfd_node_signal` takes is only reached after the release
    // below, so no sleep happens under it.
    unsafe { irq_spinlock_acquire(lock) };

    // The object may be gone: a destroyed object has had its registrations
    // reaped already, but a device teardown can race a signaler, so the lookup
    // is allowed to fail.
    // SAFETY: `dev` is live; only a live object or null comes back.
    let o = unsafe { (*dev).find_syncobj(id) };
    // SAFETY: `dev` is live; taking the list leaves an empty one behind, so no
    // borrow outlives this statement.
    let old = core::mem::take(unsafe { &mut (*dev).syncobj_ev_fds });
    let mut keep: Vec<SyncobjEventfdEntry> = Vec::with_capacity(old.len());
    // The eventfds to release, and whether each is to be signalled on the way out.
    let mut done: Vec<(*mut VfsNode, bool)> = Vec::new();
    for e in old {
        if e.id != id {
            keep.push(e); // another object's registration
            continue;
        }
        let reached = !o.is_null()
            // SAFETY: the guard above left a live object in `o`.
            && unsafe { state_of(o) }.reached(e.point);
        if notify && reached {
            done.push((e.node, true));
        } else if notify && !o.is_null() {
            keep.push(e); // still waiting for its object
        } else {
            done.push((e.node, false)); // the object is not there any more
        }
    }
    // SAFETY: `dev` is live; the list goes back under the lock, so a concurrent
    // registration cannot be lost.
    unsafe { (*dev).syncobj_ev_fds = keep };
    // SAFETY: `lock` is the device's lock, held since the acquire above.
    unsafe { irq_spinlock_release(lock) };

    for (node, signalled) in done {
        if signalled {
            // SAFETY: `node` is an eventfd this device holds a reference to.
            let _ = unsafe { eventfd_node_signal(node, 1) };
        }
        // SAFETY: `node` is the reference the registration just dropped held, and
        // it is not used again.
        unsafe { eventfd_node_put(node) };
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
    // SAFETY: `file` is the caller's live client (checked non-null above); this reads
    // its device pointer.
    let dev = unsafe { (*file).dev };
    if dev.is_null() {
        return -22;
    }
    let timeline = req.flags & DRM_SYNCOBJ_CREATE_TYPE_TIMELINE != 0;
    let signaled = !timeline && req.flags & DRM_SYNCOBJ_CREATE_SIGNALED != 0;
    // SAFETY: `dev` is the live device; this creates a syncobj on it.
    let id = unsafe { (*dev).syncobj_create(timeline, signaled, 0) };
    // SAFETY: `handle_of`'s contract: `file` is the caller's open client.
    req.handle = unsafe { handle_of(file, id) };
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
    // SAFETY: `file` is the caller's live client (checked non-null above); this
    // removes the handle from its map.
    let id = match unsafe { (*file).syncobjs.remove(&req.handle) } {
        Some(id) => id,
        None => return -2, // -ENOENT
    };
    // The object goes away when the last handle to it does.
    // SAFETY: as above — checking whether any other handle still names the id.
    let still_held = unsafe { (*file).syncobjs.values().any(|&v| v == id) };
    if !still_held {
        // SAFETY: `file` is the live client; this reads its device.
        let dev = unsafe { (*file).dev };
        if !dev.is_null() {
            // Registrations waiting on this object go with it, unsignalled.
            // SAFETY: `eventfd_reap`'s contract: `dev` is that live device.
            unsafe { eventfd_reap(dev, id, false) };
            // SAFETY: `dev` is the live device that owns the object.
            unsafe { (*dev).syncobj_destroy(id) };
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
    // SAFETY: `file` is the caller's live client (checked non-null above); this reads
    // its device pointer.
    let dev = unsafe { (*file).dev };
    if dev.is_null() {
        return -22;
    }
    // SAFETY: `object_of`'s contract: `file` is the caller's open client.
    let obj = unsafe { object_of(file, req.handle) };
    if obj.is_null() {
        return -2;
    }
    // SAFETY: `obj` is the live object; this reads its id.
    let id = unsafe { (*obj).id };

    // Exporting the same object twice hands back the same fd, as on Linux.
    // SAFETY: `dev` is the live device; this borrow of its exported-fd table is
    // consumed by the scan.
    let sync_fds = unsafe { &(*dev).sync_fds };
    for &(fd, sid, _) in sync_fds.iter() {
        if sid == id {
            req.fd = fd;
            return crate::ffi::drm_copy_out(arg, &req as *const _ as *const c_void, 16);
        }
    }

    let node = cact_mm::kmalloc(core::mem::size_of::<VfsNode>() as u32) as *mut VfsNode;
    if node.is_null() {
        return -12;
    }
    // SAFETY: `node` is the fresh `kmalloc` node just checked non-null, so zeroing
    // one `VfsNode` covers exactly that allocation.
    unsafe { core::ptr::write_bytes(node, 0, 1) };
    {
        // SAFETY: `node` is the fresh, exclusively-owned node; this borrow is
        // consumed by the field stores below.
        let node = unsafe { &mut *node };
        // SAFETY: `node.name` is a 128-byte array and the literal is
        // NUL-terminated, so `copy_cstr`'s contract is met.
        unsafe { copy_cstr(node.name.as_mut_ptr(), 128, c"syncobj".as_ptr() as *const u8) };
        node.ntype = VFS_CHARDEVICE;
        node.fops = core::ptr::addr_of_mut!(SYNC_FILE_FOPS);
    }

    let r = cact_mm::kmalloc(core::mem::size_of::<SyncFileRef>() as u32) as *mut SyncFileRef;
    if r.is_null() {
        // SAFETY: `node` is the un-published node allocated above.
        unsafe { cact_mm::kfree(node as *mut u8) };
        return -12;
    }
    {
        // SAFETY: `r` is the fresh, exclusively-owned export record; this borrow is
        // consumed by the field stores below.
        let r_ref = unsafe { &mut *r };
        r_ref.dev = dev;
        r_ref.id = id;
    }
    // SAFETY: `node` is the live node; storing its private export record.
    unsafe { (*node).priv_ = r as *mut c_void };

    // SAFETY: `alloc_fd`'s contract wants a live node to install as the new fd's
    // file.
    let fd = unsafe { alloc_fd(node as *mut c_void) };
    if fd < 0 {
        // SAFETY: `r` and `node` were allocated above and not published.
        unsafe { cact_mm::kfree(r as *mut u8) };
        // SAFETY: as above.
        unsafe { cact_mm::kfree(node as *mut u8) };
        return -24; // -EMFILE
    }
    // SAFETY: `dev` is the live device; recording the new export under the lock-free
    // per-device table the other paths also touch.
    unsafe { (*dev).sync_fds.push((fd, id, node)) };
    req.fd = fd;
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
    // SAFETY: `file` is the caller's live client (checked non-null above); this reads
    // its device pointer.
    let dev = unsafe { (*file).dev };
    if dev.is_null() {
        return -22;
    }
    let mut id = 0u32;
    // SAFETY: `dev` is the live device; this borrow of its exported-fd table is
    // consumed by the scan.
    let sync_fds = unsafe { &(*dev).sync_fds };
    for &(fd, sid, _) in sync_fds.iter() {
        if fd == req.fd {
            id = sid;
            break;
        }
    }
    if id == 0 {
        return -22; // not a sync file this device exported
    }
    // SAFETY: `handle_of`'s contract: `file` is the caller's open client.
    req.handle = unsafe { handle_of(file, id) };
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
    let mut handles: Vec<u32> = vec![0u32; n];
    // SAFETY: `read_u32s`'s contract: `req.handles` is the user array and `handles`
    // has exactly `n` elements.
    if !unsafe { read_u32s(req.handles, &mut handles) } {
        return -14;
    }
    for h in handles {
        // SAFETY: `object_of`'s contract: `file` is the caller's open client.
        let o = unsafe { object_of(file, h) };
        if o.is_null() {
            return -2;
        }
        // SAFETY: `o` is the live object; this reads its flavour.
        if unsafe { state_of(o) }.timeline {
            return -22; // use the timeline ioctls for a timeline object
        }
        // SAFETY: as above — its id, for the registrations waiting on it.
        let id = unsafe { (*o).id };
        // SAFETY: `o` is the live object; this reads its state.
        let mut st = unsafe { state_of(o) };
        if on {
            st.signal();
        } else {
            st.reset();
        }
        // SAFETY: as above — writing back the bit that moved.
        unsafe { set_state(o, st) };
        if on {
            // A signal is the moment a registered eventfd can fire.
            // SAFETY: `file` is the caller's open client; this reads its device.
            let dev = unsafe { (*file).dev };
            // SAFETY: `eventfd_reap`'s contract: `dev` is that live device.
            unsafe { eventfd_reap(dev, id, true) };
        }
    }
    0
}

/// `DRM_IOCTL_SYNCOBJ_TRANSFER` — move the source's state onto the destination.
///
/// The destination chooses the shape: `dst_point != 0` means a timeline
/// destination, whose point moves to `dst_point` exactly as a
/// `TIMELINE_SIGNAL` there would; `dst_point == 0` means a binary destination,
/// which becomes signalled.  A destination of the other flavour is `-EINVAL`,
/// and so is a source that has not reached the state being asked for — this
/// kernel holds no pending fences, so there is nothing else a transfer could
/// put on the destination.
#[no_mangle]
pub extern "C" fn syncobj_ioctl_transfer(file: *mut DrmFile, arg: *mut c_void) -> c_int {
    if file.is_null() {
        return -22;
    }
    let mut req = SyncobjTransfer {
        src_handle: 0,
        dst_handle: 0,
        src_point: 0,
        dst_point: 0,
        flags: 0,
        pad: 0,
    };
    if drm_copy_in(&mut req as *mut _ as *mut c_void, DRM_IOCTL_SYNCOBJ_TRANSFER, arg, 32) != 0 {
        return -22;
    }
    if req.pad != 0 {
        return -22;
    }
    // The source must carry the state that is being moved.
    // SAFETY: `find_state`'s contract: `file` is an open client.
    let rc = unsafe { find_state(file, req.src_handle, req.src_point, req.flags) };
    if rc != 0 {
        return rc;
    }
    // SAFETY: `object_of`'s contract: `file` is the caller's open client.
    let dst = unsafe { object_of(file, req.dst_handle) };
    if dst.is_null() {
        return -2; // -ENOENT
    }
    // SAFETY: `dst` is the live destination; this reads its state.
    let mut st = unsafe { state_of(dst) };
    if st.take_transfer(req.dst_point).is_err() {
        return -22; // a point belongs to a timeline object, the bit to a binary one
    }
    // SAFETY: `dst` is the live destination; only the fields the transfer moved
    // are written back.
    unsafe { set_state(dst, st) };
    // SAFETY: `dst` is the live destination; this reads its id for the reap.
    let id = unsafe { (*dst).id };
    // SAFETY: `file` is the caller's open client; this reads its device.
    let dev = unsafe { (*file).dev };
    // SAFETY: `eventfd_reap`'s contract: `dev` is that live device, and the
    // destination just changed.
    unsafe { eventfd_reap(dev, id, true) };
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

    let mut handles: Vec<u32> = vec![0u32; n];
    // SAFETY: `read_u32s`'s contract: `req.handles` is the user array and `handles`
    // has exactly `n` elements.
    if !unsafe { read_u32s(req.handles, &mut handles) } {
        return -14;
    }
    let mut objs: Vec<*mut Syncobj> = Vec::with_capacity(n);
    for &h in handles.iter() {
        // SAFETY: `object_of`'s contract: `file` is the caller's open client.
        let o = unsafe { object_of(file, h) };
        if o.is_null() {
            return -2;
        }
        // SAFETY: `o` is the live object; this reads its flavour.
        if unsafe { state_of(o) }.timeline {
            return -22; // timeline objects use TIMELINE_WAIT
        }
        objs.push(o);
    }

    let mut ticks = wait_ticks(req.timeout_nsec);
    loop {
        let mut done = 0usize;
        let mut first = 0u32;
        for (i, &o) in objs.iter().enumerate() {
            // SAFETY: `o` is a live object collected above; a binary object is
            // done exactly when its bit is set, which is what `reached(0)` asks.
            if unsafe { state_of(o) }.reached(0) {
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
        // SAFETY: `sched_sleep_ticks` is a kernel service that suspends the calling
        // task for the requested number of ticks.
        unsafe { sched_sleep_ticks(1) };
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
    let mut handles: Vec<u32> = vec![0u32; n];
    let mut points: Vec<u64> = vec![0u64; n];
    // SAFETY: `read_u32s`'s contract: `req.handles` is the user array and `handles`
    // has exactly `n` elements.
    if !unsafe { read_u32s(req.handles, &mut handles) } {
        return -14;
    }
    // SAFETY: `read_u64s`'s contract: `req.points` is the user array and `points` has
    // exactly `n` elements.
    if !unsafe { read_u64s(req.points, &mut points) } {
        return -14;
    }
    for i in 0..n {
        // SAFETY: `object_of`'s contract: `file` is the caller's open client.
        let o = unsafe { object_of(file, handles[i]) };
        if o.is_null() {
            return -2;
        }
        // SAFETY: `o` is the live object; this reads its state.
        let mut st = unsafe { state_of(o) };
        if !st.timeline {
            return -22;
        }
        // A timeline point only moves forward — and when it does not, nothing
        // registered on this object can have become ready.
        if st.advance(points[i]) {
            // SAFETY: as above — writing back the point that moved.
            unsafe { set_state(o, st) };
            // SAFETY: as above — its id, for the registrations waiting on it.
            let id = unsafe { (*o).id };
            // SAFETY: `file` is the caller's open client; this reads its device.
            let dev = unsafe { (*file).dev };
            // SAFETY: `eventfd_reap`'s contract: `dev` is that live device.
            unsafe { eventfd_reap(dev, id, true) };
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
    let mut handles: Vec<u32> = vec![0u32; n];
    // SAFETY: `read_u32s`'s contract: `req.handles` is the user array and `handles`
    // has exactly `n` elements.
    if !unsafe { read_u32s(req.handles, &mut handles) } {
        return -14;
    }
    let mut points: Vec<u64> = vec![0u64; n];
    for i in 0..n {
        // SAFETY: `object_of`'s contract: `file` is the caller's open client.
        let o = unsafe { object_of(file, handles[i]) };
        if o.is_null() {
            return -2;
        }
        // SAFETY: `o` is the live object; this reads its flavour.
        if !unsafe { state_of(o) }.timeline {
            return -22;
        }
        // SAFETY: `o` is the live object; this reads the point to report.
        points[i] = unsafe { state_of(o) }.point;
    }
    // SAFETY: `write_u64s`'s contract: `req.points` is the user array and `points`
    // has exactly `n` elements.
    if !unsafe { write_u64s(req.points, &points) } {
        return -14;
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

    let mut handles: Vec<u32> = vec![0u32; n];
    let mut points: Vec<u64> = vec![0u64; n];
    // SAFETY: `read_u32s`'s contract: `req.handles` is the user array and `handles`
    // has exactly `n` elements.
    if !unsafe { read_u32s(req.handles, &mut handles) } {
        return -14;
    }
    // SAFETY: `read_u64s`'s contract: `req.points` is the user array and `points` has
    // exactly `n` elements.
    if !unsafe { read_u64s(req.points, &mut points) } {
        return -14;
    }
    let mut objs: Vec<*mut Syncobj> = Vec::with_capacity(n);
    for &h in handles.iter() {
        // SAFETY: `object_of`'s contract: `file` is the caller's open client.
        let o = unsafe { object_of(file, h) };
        if o.is_null() {
            return -2;
        }
        // SAFETY: `o` is the live object; this reads its flavour.
        if !unsafe { state_of(o) }.timeline {
            return -22;
        }
        objs.push(o);
    }

    let mut ticks = wait_ticks(req.timeout_nsec);
    loop {
        let mut done = 0usize;
        let mut first = 0u32;
        for (i, &o) in objs.iter().enumerate() {
            // The wait asks for a point the object has reached, and `points[i] == 0`
            // means "no particular point" — so this compares the raw point rather
            // than going through `reached()`, whose 0 means "any point at all"
            // (the fence-shaped question a transfer and an eventfd ask).
            // SAFETY: `o` is a live object collected above; this reads its point.
            if unsafe { state_of(o) }.point >= points[i] {
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
        // SAFETY: `sched_sleep_ticks` is a kernel service that suspends the calling
        // task for the requested number of ticks.
        unsafe { sched_sleep_ticks(1) };
    }
    crate::ffi::drm_copy_out(arg, &req as *const _ as *const c_void, 48)
}

/// `DRM_IOCTL_SYNCOBJ_EVENTFD` — have `fd` incremented by one when the syncobj
/// reaches `point`.
///
/// `fd` must be an eventfd (`-EBADF` when it is not open, `-EINVAL` when it is
/// not an eventfd); the registration keeps it open by reference, so signalling
/// works even if the registering process has closed that fd in the meantime.
/// A registration on a state that is already there fires inside this call, so
/// the caller never observes "registered but never signalled" for an object
/// that had already got there.
///
/// `WAIT_AVAILABLE` is accepted and has no effect: a point that exists is a
/// point that is signalled here, so waiting for availability and waiting for
/// the signal are the same wait.
#[no_mangle]
pub extern "C" fn syncobj_ioctl_eventfd(file: *mut DrmFile, arg: *mut c_void) -> c_int {
    if file.is_null() {
        return -22;
    }
    let mut req = SyncobjEventfd {
        handle: 0,
        flags: 0,
        point: 0,
        fd: -1,
        pad: 0,
    };
    if drm_copy_in(&mut req as *mut _ as *mut c_void, DRM_IOCTL_SYNCOBJ_EVENTFD, arg, 24) != 0 {
        return -22;
    }
    if req.pad != 0 {
        return -22;
    }
    if req.flags & !DRM_SYNCOBJ_WAIT_FLAGS_WAIT_AVAILABLE != 0 {
        return -22;
    }
    // SAFETY: `object_of`'s contract: `file` is the caller's open client.
    let o = unsafe { object_of(file, req.handle) };
    if o.is_null() {
        return -2; // -ENOENT
    }
    // SAFETY: `o` is the live object; this reads its state, which decides whether
    // `req.point` is a fair question to ask of it.
    if !unsafe { state_of(o) }.takes_point(req.point) {
        return -22; // a point belongs to a timeline object
    }
    // SAFETY: as above — its id, which is what a registration is keyed on.
    let id = unsafe { (*o).id };

    let mut node: *mut VfsNode = core::ptr::null_mut();
    // SAFETY: `eventfd_node_get` looks `req.fd` up in the calling task's table
    // and, on success, hands back a reference this device now owns.
    let rc = unsafe { eventfd_node_get(req.fd, &mut node as *mut *mut VfsNode) };
    if rc != 0 {
        return rc;
    }

    // SAFETY: `file` is the caller's open client; this reads its device.
    let dev = unsafe { (*file).dev };
    if dev.is_null() {
        // SAFETY: `node` is the reference just taken and nothing else owns it yet.
        unsafe { eventfd_node_put(node) };
        return -22;
    }
    // SAFETY: `dev` is the live device; the registration goes on under the device
    // lock, so a concurrent reap cannot drop it.
    unsafe {
        eventfd_register(
            dev,
            SyncobjEventfdEntry {
                id,
                point: req.point,
                node,
            },
        )
    };
    // An object that has already reached the point fires right here; otherwise
    // the entry stays until a SIGNAL/TIMELINE_SIGNAL/TRANSFER moves it.
    // SAFETY: `eventfd_reap`'s contract: `dev` is that live device.
    unsafe { eventfd_reap(dev, id, true) };
    0
}
