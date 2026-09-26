//! Per-open DRM client state and the VFS node operations.
//!
//! A card fd carries a client (GEM handle table, framebuffers, event queue)
//! that belongs to one open() and is shared by dup()ed descriptors, so the
//! operations live in a `vfs_file_ops_t` and are reached through `file_t.priv`.
//!
//! The handle table, the framebuffer list and the event queue are all
//! allocator-backed, and the device's client list is a `Vec`, so none of them
//! is capped.

use alloc::boxed::Box;
use alloc::collections::{BTreeMap, VecDeque};
use alloc::vec::Vec;
use core::ffi::{c_int, c_void};

use crate::gem::{drm_gem_find_map_offset, drm_gem_memfd, drm_gem_unref};
use crate::ioctl::drm_ioctl_dispatch;
use crate::kms::framebuffer::drm_fb_handle_release;
use crate::structs::{DrmDevice, DrmFile, EventVblank, FILE_NAME_MAX};
use crate::vfs::{
    copy_cstr, File, VfsFileOps, VfsNode, VfsOps, VFS_CHARDEVICE, VFS_POLLERR, VFS_POLLIN,
    VFS_POLLOUT,
};

/// The node-ops table of a DRM card: only the mmap hook, which is how
/// `DRM_IOCTL_MODE_MAP_DUMB` offsets reach `do_mmap()`.
static mut DRM_NODE_OPS: VfsOps = VfsOps {
    before: [core::ptr::null_mut(); 17],
    mmap_backing: Some(drm_node_mmap_backing),
    after: [core::ptr::null_mut(); 7],
};

/// The file-ops table: every operation on an open card fd.
static mut DRM_NODE_FOPS: VfsFileOps = VfsFileOps {
    read: Some(drm_node_read),
    write: None,
    ioctl: Some(drm_node_ioctl),
    poll: Some(drm_node_poll),
    open: Some(drm_node_open_file),
    release: Some(drm_node_release),
};

/* ── client lifecycle ───────────────────────────────────────────────────── */

unsafe fn drm_client_create(dev: *mut DrmDevice, is_render: i32) -> *mut DrmFile {
    // SAFETY: `dev` is the caller's live device (devfs passes one of our own nodes,
    // whose priv is a device pointer); the new client is pushed into its client list
    // before its raw pointer escapes, and this borrow is exclusive for the setup.
    let dev = unsafe { &mut *dev };
    /* The first card open becomes master; a render node never does. */
    let is_master = if is_render == 0 && dev.clients.is_empty() {
        1
    } else {
        0
    };
    dev.next_magic += 1;
    let magic = dev.next_magic;

    let mut name = [0u8; FILE_NAME_MAX];
    if is_render != 0 {
        // SAFETY: `name` is a `FILE_NAME_MAX`-byte local and the literal is
        // NUL-terminated, so `copy_cstr`'s contract is met.
        unsafe {
            copy_cstr(
                name.as_mut_ptr(),
                FILE_NAME_MAX,
                c"render".as_ptr() as *const u8,
            );
        }
    } else {
        // SAFETY: as above, with the `c"card"` literal.
        unsafe { copy_cstr(name.as_mut_ptr(), FILE_NAME_MAX, c"card".as_ptr() as *const u8) };
    }

    {

        let f = Box::new(DrmFile {
            dev,
            is_render,
            is_master,
            authenticated: 1,
            magic,
            name,
            handles: BTreeMap::new(),
            next_handle: 0,
            fbs: Vec::new(),
            syncobjs: BTreeMap::new(),
            next_syncobj_handle: 0,
            events: VecDeque::new(),
        });
        let f = Box::into_raw(f);
        dev.clients.push(f);
        f
    }
}

/// Release a client: the driver's `close` first, then everything the client
/// holds, then the client itself.
pub(crate) unsafe fn drm_client_destroy(f: *mut DrmFile) {
    if f.is_null() {
        return;
    }
    // SAFETY: `f` is a client this crate allocated with `Box::into_raw`, so this
    // `dev` field read is in bounds.
    let dev = unsafe { (*f).dev };

    /* Tell the driver before anything the client owns goes away: a 3D driver
     * destroys its per-open contexts here and may still reference resources
     * whose handles are about to be dropped. */
    if !dev.is_null() {
        // SAFETY: `dev` is the client's live device; this copies the ops pointer.
        let ops = unsafe { (*dev).ops };
        if !ops.is_null() {
            // SAFETY: `ops` is the driver's live ops table; this copies the close fn
            // pointer only.
            let close = unsafe { (*ops).close };
            if let Some(close) = close {
                close(dev, f);
            }
        }
    }

    // SAFETY: `f` is the live client; this borrow of its handle map is consumed by
    // the copy below.
    let handles: Vec<*mut crate::structs::GemObject> =
        unsafe { (*f).handles.values().copied().collect() };
    // SAFETY: as above — clearing the handle map after the copy.
    unsafe { (*f).handles.clear() };
    for obj in handles {
        if !obj.is_null() {
            drm_gem_unref(obj);
        }
    }

    // SAFETY: `f` is the live client; this clones its framebuffer-id list.
    let fbs: Vec<u32> = unsafe { (*f).fbs.clone() };
    for id in fbs {
        drm_fb_handle_release(f, id);
    }

    if !dev.is_null() {
        // SAFETY: `dev` is the client's live device; dropping `f` from its client
        // list before the Box is reclaimed.
        unsafe { (*dev).clients.retain(|&p| p != f) };
    }
    // SAFETY: `f` came from `Box::into_raw` above and has been removed from the
    // client list, so reclaiming the Box here is the matching free.
    unsafe { drop(Box::from_raw(f)) };
}

/* ── vfs node ops ───────────────────────────────────────────────────────── */

extern "C" fn drm_node_open_file(node: *mut VfsNode, file: *mut File) {
    if node.is_null() || file.is_null() {
        return;
    }
    // SAFETY: devfs passes one of our own nodes, so this `priv_` read is in bounds.
    let dev = unsafe { (*node).priv_ as *mut DrmDevice };
    if dev.is_null() {
        return;
    }
    // SAFETY: `dev` is the node's live device; this only compares the node address
    // against the render-node pointer.
    let is_render = if node == unsafe { (*dev).render_node } { 1 } else { 0 };
    /* open() has no way to fail here (vfs_file_ops_t::open is void): worst case
     * the client is not created and every later operation on this fd fails
     * cleanly. */
    // SAFETY: `drm_client_create`'s contract: `dev` is a live device and the new
    // client is registered in its list before the pointer escapes.
    let client = unsafe { drm_client_create(dev, is_render) };
    // SAFETY: `file` is the kernel's new file, so storing its private data is in
    // bounds.
    unsafe { (*file).priv_ = client as *mut c_void };
}

extern "C" fn drm_node_release(node: *mut VfsNode, file: *mut File) {
    let _ = node;
    if file.is_null() {
        return;
    }
    // SAFETY: `file` is the kernel's file and its private data is the client created
    // by `drm_node_open_file`.
    let client = unsafe { (*file).priv_ as *mut DrmFile };
    // SAFETY: `drm_client_destroy`'s contract: `client` is that client, which is not
    // used again after this call.
    unsafe { drm_client_destroy(client) };
    // SAFETY: clearing the file's private data before the file is released.
    unsafe { (*file).priv_ = core::ptr::null_mut() };
}

/// `read(2)` on a card fd returns queued DRM events, as libdrm's
/// `drmHandleEvent()` expects.  Non-blocking: the caller (a poll loop) retries
/// on EAGAIN.
extern "C" fn drm_node_read(
    node: *mut VfsNode,
    priv_: *mut c_void,
    off: u32,
    size: u32,
    buf: *mut u8,
) -> c_int {
    let _ = (node, off);
    let f = priv_ as *mut DrmFile;
    if f.is_null() {
        return -1;
    }
    if size < core::mem::size_of::<EventVblank>() as u32 {
        return -1;
    }
    // SAFETY: `f` is this fd's own client state, so popping its event queue is in
    // bounds.
    let ev = match unsafe { (*f).events.pop_front() } {
        Some(ev) => ev,
        None => return -11, // -EAGAIN
    };
    // SAFETY: `buf` is the caller's `size`-byte buffer and `size >=
    // size_of::<EventVblank>()` was checked above, so the copy fits.
    unsafe {
        core::ptr::copy_nonoverlapping(
            &ev as *const EventVblank as *const u8,
            buf,
            core::mem::size_of::<EventVblank>(),
        );
    }
    core::mem::size_of::<EventVblank>() as c_int
}

extern "C" fn drm_node_poll(node: *mut VfsNode, priv_: *mut c_void, events: u32) -> c_int {
    let _ = node;
    let f = priv_ as *mut DrmFile;
    if f.is_null() {
        return VFS_POLLERR as c_int;
    }
    let mut rev = 0u32;
    // SAFETY: the client is this fd's own state.
    unsafe {
        if !(*f).events.is_empty() {
            rev |= VFS_POLLIN;
        }
    }
    if events & VFS_POLLOUT != 0 {
        rev |= VFS_POLLOUT;
    }
    rev as c_int
}

/// `mmap()` of a card fd: `DRM_IOCTL_MODE_MAP_DUMB` handed userspace a faked
/// offset, so resolve it back to the GEM object it names and let `do_mmap()`
/// install that object's own frames.
extern "C" fn drm_node_mmap_backing(
    node: *mut VfsNode,
    off: u32,
    len: u32,
    backing_out: *mut c_int,
    obj_off_out: *mut u32,
) -> c_int {
    if node.is_null() {
        return -1;
    }
    // SAFETY: devfs passes one of our own nodes (checked non-null above), so this
    // `priv_` read is in bounds.
    let dev = unsafe { (*node).priv_ as *mut DrmDevice };
    if dev.is_null() {
        return -1;
    }

    let obj = drm_gem_find_map_offset(dev, off);
    if obj.is_null() {
        return -1;
    }

    // SAFETY: `backing_out` is the caller's out-parameter for the memfd fd number.
    unsafe { *backing_out = drm_gem_memfd(obj) };
    // SAFETY: `obj_off_out` is the caller's out-parameter for the in-object offset.
    unsafe { *obj_off_out = 0 };

    /* Only the whole object is ever mapped: an offset inside it would need a
     * second memfd view, and DRM's dumb-buffer API never asks for that. */
    let _ = len;
    // SAFETY: `backing_out` was just written above, so this reads an initialised
    // value.
    if unsafe { *backing_out } > 0 {
        0
    } else {
        -1
    }
}

/// The ioctl path needs the per-open client, so it is installed as a file-op;
/// the node-ops table only carries `mmap_backing`.
extern "C" fn drm_node_ioctl(
    node: *mut VfsNode,
    priv_: *mut c_void,
    cmd: u32,
    arg: *mut c_void,
) -> c_int {
    let _ = node;
    let f = priv_ as *mut DrmFile;
    if f.is_null() {
        return -1;
    }
    // SAFETY: the client is this fd's own state.
    unsafe {
        if (*f).dev.is_null() {
            return -1;
        }
    }
    drm_ioctl_dispatch(f, cmd, arg)
}

/* ── devfs plumbing ─────────────────────────────────────────────────────── */

#[no_mangle]
pub extern "C" fn drm_fill_node(n: *mut VfsNode, name: *const u8, dev: *mut DrmDevice) {
    if n.is_null() {
        return;
    }
    // SAFETY: the caller owns the freshly allocated node, so zeroing one `VfsNode`
    // is in bounds.
    unsafe { core::ptr::write_bytes(n, 0, 1) };
    // SAFETY: `n` is that node; this borrow is consumed by the field stores below.
    let n = unsafe { &mut *n };
    // SAFETY: `n.name` is a 128-byte array and `copy_cstr` requires `dst` writable
    // for `cap` bytes; `name` is the caller's NUL-terminated string.
    unsafe { copy_cstr(n.name.as_mut_ptr(), 128, name) };
    n.ntype = VFS_CHARDEVICE;
    n.ops = core::ptr::addr_of_mut!(DRM_NODE_OPS);
    n.fops = core::ptr::addr_of_mut!(DRM_NODE_FOPS);
    n.priv_ = dev as *mut c_void;
    n.mode = 0o600;
}
