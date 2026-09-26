//! GEM buffer objects, client handle tables, mmap offsets and the dumb-buffer
//! ioctls, plus PRIME buffer sharing.
//!
//! Objects are memfd-backed on purpose: a memfd already provides a page array,
//! an owner reference count, MAP_SHARED accounting and a fork()-safe lifetime,
//! so mmap(), munmap(), dup(), fork() and PRIME all work through
//! already-exercised paths.  The GEM object is therefore only a description
//! (size, dumb geometry, map offset) wrapped around a memfd handle.
//!
//! Handles, map offsets and global names are all allocator-backed maps, so a
//! client may hold as many handles and a device hand out as many offsets as it
//! likes.

use core::ffi::{c_int, c_void};

use crate::ffi::{drm_copy_in, drm_copy_out, printk};
use crate::structs::{DrmDevice, DrmFile, GemObject, IrqSpinlock};

extern "C" {

    fn irq_spinlock_acquire(lock: *mut IrqSpinlock);
    fn irq_spinlock_release(lock: *mut IrqSpinlock);

    /* fs/memfd/memfd.h — the shared RAM object every GEM buffer is built on. */
    fn memfd_create(name: *const u8, name_len: u32, flags: c_int) -> c_int;
    fn memfd_truncate(handle: c_int, size: u32) -> c_int;
    fn memfd_close(handle: c_int) -> c_int;
    fn memfd_ref(handle: c_int) -> c_int;
    fn memfd_get_page(handle: c_int, idx: u32) -> *mut c_void;
    fn memfd_size(handle: c_int) -> c_int;
    fn memfd_fd_handle(fd: c_int) -> c_int;
    fn memfd_vnode_from_handle(handle: c_int, name: *const u8, size: u32) -> *mut c_void;

    /* kernel/core/syscall/helper.h */
    fn alloc_fd(node: *mut c_void) -> c_int;
}

/// Address of the device's bookkeeping lock.
#[inline]
unsafe fn lock_ptr(dev: *mut DrmDevice) -> *mut IrqSpinlock {
    // SAFETY: `dev` is a live device owned by the caller; `addr_of_mut!` only
    // forms the address of its `lock` field and never creates a reference.
    unsafe { core::ptr::addr_of_mut!((*dev).lock) }
}

/* ── object lifecycle ───────────────────────────────────────────────────── */

/// `drm_gem_link` — link an object into the device's list.  Shared with PRIME
/// import, which adopts an object it did not allocate.  The caller holds the
/// device lock.
#[no_mangle]
pub extern "C" fn drm_gem_link(dev: *mut DrmDevice, obj: *mut GemObject) {
    if dev.is_null() || obj.is_null() {
        return;
    }
    // SAFETY: caller's live device and object.
    unsafe {
        (*dev).gem_list.push(obj);
    }
}

/// Create an object of `size` bytes (rounded up to a page) backed by a memfd,
/// and return it with one reference held.  NULL on failure.
#[no_mangle]
pub extern "C" fn drm_gem_create(dev: *mut DrmDevice, size: u32) -> *mut GemObject {
    if dev.is_null() || size == 0 {
        return core::ptr::null_mut();
    }
    let obj = cact_mm::kmalloc(core::mem::size_of::<GemObject>() as u32) as *mut GemObject;
    if obj.is_null() {
        return core::ptr::null_mut();
    }
    // SAFETY: `obj` is the fresh `kmalloc` object just checked non-null, so zeroing
    // one `GemObject` covers exactly that allocation.
    unsafe { core::ptr::write_bytes(obj, 0, 1) };

    let rounded = (size + 4095) & !4095u32;

    // SAFETY: `memfd_create` is a kernel C service; the name is a NUL-terminated
    // literal and the flags are the values the C path used.
    let h = unsafe { memfd_create(c"drm-gem".as_ptr() as *const u8, 7, 0) };
    if h <= 0 {
        // SAFETY: `obj` is the object allocated above and not yet published.
        unsafe { cact_mm::kfree(obj as *mut u8) };
        return core::ptr::null_mut();
    }
    // SAFETY: `h` is the live memfd just created and `rounded` its requested size.
    if unsafe { memfd_truncate(h, rounded) } != 0 {
        // SAFETY: `h` is the live memfd; `obj` is the un-published object.
        unsafe { memfd_close(h) };
        // SAFETY: as above.
        unsafe { cact_mm::kfree(obj as *mut u8) };
        return core::ptr::null_mut();
    }

    {
        // SAFETY: `obj` is the fresh, exclusively-owned object; this borrow is
        // consumed by the field stores below and ends before the driver hook.
        let obj_ref = unsafe { &mut *obj };
        obj_ref.dev = dev;
        obj_ref.memfd = h;
        obj_ref.size = rounded;
        obj_ref.refcount = 1;
    }

    /* Let the driver refuse or constrain the object (e.g. a device that needs
     * physically contiguous scanout). */
    // SAFETY: `dev` is the caller's live device; this copies its ops pointer.
    let ops = unsafe { (*dev).ops };
    if !ops.is_null() {
        // SAFETY: `ops` is the driver's live ops table; this copies the `gem_create`
        // fn pointer only.
        let create = unsafe { (*ops).gem_create };
        if let Some(create) = create {
            // `create` is the plain `extern "C"` driver hook and `dev`/`obj` are
            // the live device and object it expects.
            if create(dev, obj) != 0 {
                // SAFETY: on refusal the object and its memfd are still ours.
                unsafe { memfd_close(h) };
                // SAFETY: as above.
                unsafe { cact_mm::kfree(obj as *mut u8) };
                return core::ptr::null_mut();
            }
        }
    }

    // SAFETY: `lock_ptr`'s contract: `dev` is the caller's live device.
    let lock = unsafe { lock_ptr(dev) };
    // SAFETY: `lock` is the device's lock word; the pair brackets the link update.
    unsafe { irq_spinlock_acquire(lock) };
    drm_gem_link(dev, obj);
    // SAFETY: as above — releasing the same lock.
    unsafe { irq_spinlock_release(lock) };
    obj
}

/// Take a reference.
#[no_mangle]
pub extern "C" fn drm_gem_ref(obj: *mut GemObject) -> c_int {
    if obj.is_null() {
        return -1;
    }
    // SAFETY: `obj` is the caller's live object (checked non-null above); this
    // borrow is consumed by the counter update and the read that follows it.
    let obj = unsafe { &mut *obj };
    obj.refcount += 1;
    obj.refcount
}

/// Drop a reference; the last one frees the object and its memfd.
#[no_mangle]
pub extern "C" fn drm_gem_unref(obj: *mut GemObject) -> c_int {
    if obj.is_null() {
        return -1;
    }
    // SAFETY: `obj` is the caller's live object (checked non-null above), so this
    // reference-count read is in bounds.
    if unsafe { (*obj).refcount } == 0 {
        return -1;
    }
    // SAFETY: as above — dropping one reference.
    unsafe { (*obj).refcount -= 1 };
    // SAFETY: as above — re-reading the count.
    if unsafe { (*obj).refcount } > 0 {
        // SAFETY: as above.
        return unsafe { (*obj).refcount };
    }

    // SAFETY: `obj` is the live object; this reads its device pointer.
    let dev = unsafe { (*obj).dev };
    // SAFETY: `dev` is the object's live device; this copies its ops pointer.
    let ops = unsafe { (*dev).ops };
    if !ops.is_null() {
        // SAFETY: `ops` is the driver's live ops table; this copies the `gem_free`
        // fn pointer only.
        let free = unsafe { (*ops).gem_free };
        if let Some(free) = free {
            // `free` is the plain `extern "C"` driver hook and `obj` the live object
            // it is releasing.
            free(obj);
        }
    }

    // SAFETY: `lock_ptr`'s contract: `dev` is the object's live device.
    let lock = unsafe { lock_ptr(dev) };
    // SAFETY: `lock` is the device's lock word; the pair brackets the unlink below.
    unsafe { irq_spinlock_acquire(lock) };
    /* Drop the map offsets first: a client may still have the object mapped, but
     * no new mmap may resolve to it.  Same for its global name. */
    // SAFETY: `dev` is the live device; this drops the object from its map-offset
    // table while the lock is held.
    unsafe { (*dev).map_offsets.retain(|_, &mut v| v != obj) };
    // SAFETY: as above — the flink table.
    unsafe { (*dev).flink.retain(|_, &mut v| v != obj) };
    // SAFETY: as above — the object list.
    unsafe { (*dev).gem_list.retain(|&p| p != obj) };
    // SAFETY: as above — releasing the same lock.
    unsafe { irq_spinlock_release(lock) };

    // SAFETY: `obj` is the live object; this reads its memfd handle.
    let memfd = unsafe { (*obj).memfd };
    if memfd > 0 {
        // SAFETY: that memfd was created by `drm_gem_create` and is closed once here.
        unsafe { memfd_close(memfd) };
    }
    // SAFETY: the object's last reference was just dropped, so reclaiming its
    // allocation is the matching free.
    unsafe { cact_mm::kfree(obj as *mut u8) };
    0
}

#[no_mangle]
pub extern "C" fn drm_gem_size(obj: *mut GemObject) -> u32 {
    if obj.is_null() {
        0
    } else {
        // SAFETY: caller's object.
        unsafe { (*obj).size }
    }
}

#[no_mangle]
pub extern "C" fn drm_gem_memfd(obj: *mut GemObject) -> c_int {
    if obj.is_null() {
        -1
    } else {
        // SAFETY: caller's object.
        unsafe { (*obj).memfd }
    }
}

/// Kernel-visible address of `off` inside the object.
///
/// Only the bytes up to the end of *that frame* are addressable through the
/// returned pointer: memfd frames are allocated individually and are not
/// contiguous, so callers must walk frame by frame (`off = i * 4096`).
#[no_mangle]
pub extern "C" fn drm_gem_vaddr(obj: *mut GemObject, off: u32) -> *mut c_void {
    if obj.is_null() {
        return core::ptr::null_mut();
    }
    // SAFETY: `obj` is the caller's live object (checked non-null above), so this
    // `size` read is in bounds.
    if off >= unsafe { (*obj).size } {
        return core::ptr::null_mut();
    }
    // SAFETY: `obj` is the live object; this reads its memfd handle.
    let memfd = unsafe { (*obj).memfd };
    // SAFETY: `memfd` is that object's live memfd and `off / 4096` its page index.
    let page = unsafe { memfd_get_page(memfd, off / 4096) } as *mut u8;
    if page.is_null() {
        return core::ptr::null_mut();
    }
    // SAFETY: `page` is a live 4 KiB frame and `off & 4095` indexes within it.
    unsafe { page.add((off & 4095) as usize) as *mut c_void }
}

/* ── mmap offsets ───────────────────────────────────────────────────────── */

/// Resolve a faked mmap offset back to its object (used by the VFS mmap hook).
#[no_mangle]
pub extern "C" fn drm_gem_find_map_offset(dev: *mut DrmDevice, offset: u32) -> *mut GemObject {
    if dev.is_null() {
        return core::ptr::null_mut();
    }
    // SAFETY: read-only walk of the caller's device.
    unsafe {
        match (*dev).map_offsets.get(&offset) {
            Some(&obj) => obj,
            None => core::ptr::null_mut(),
        }
    }
}

/// The caller holds the device lock.
unsafe fn drm_gem_alloc_map_offset(dev: *mut DrmDevice, obj: *mut GemObject) -> u32 {
    // SAFETY: the caller holds the device lock (see the function's contract), so
    // `map_offsets` has no concurrent writer; `dev` is live and this borrow is
    // exclusive.
    let dev = unsafe { &mut *dev };
    let off = dev.next_map_offset;
    dev.next_map_offset = off.wrapping_add(0x1000_0000); // stay page aligned
    dev.map_offsets.insert(off, obj);
    off
}

/// An offset a client can pass to mmap(2).  It is stable for the object's
/// lifetime, so asking repeatedly returns the same one.
#[no_mangle]
pub extern "C" fn drm_gem_map_offset(
    dev: *mut DrmDevice,
    obj: *mut GemObject,
    offset_out: *mut u64,
) -> c_int {
    if dev.is_null() || obj.is_null() || offset_out.is_null() {
        return -22;
    }
    // SAFETY: `obj` is the caller's live object (checked non-null above); this reads
    // its cached map offset.
    let mut off = unsafe { (*obj).map_offset };
    if off == 0 {
        // SAFETY: `lock_ptr`'s contract: `dev` is the caller's live device.
        let lock = unsafe { lock_ptr(dev) };
        // SAFETY: `lock` is the device's lock word; the pair brackets the allocation.
        unsafe { irq_spinlock_acquire(lock) };
        // SAFETY: `drm_gem_alloc_map_offset`'s contract: the device lock is held.
        off = unsafe { drm_gem_alloc_map_offset(dev, obj) };
        // SAFETY: as above — releasing the same lock.
        unsafe { irq_spinlock_release(lock) };
        // SAFETY: `obj` is the live object; caching the freshly-allocated offset.
        unsafe { (*obj).map_offset = off };
    }
    // SAFETY: `offset_out` is the caller's out-parameter for the offset.
    unsafe { *offset_out = off as u64 };
    0
}

/* ── handle table ───────────────────────────────────────────────────────── */

/// Give `obj` a per-client handle.
#[no_mangle]
pub extern "C" fn drm_gem_handle_create(
    file: *mut DrmFile,
    obj: *mut GemObject,
    handle_out: *mut u32,
) -> c_int {
    if file.is_null() || obj.is_null() || handle_out.is_null() {
        return -1;
    }
    // SAFETY: `file` is the caller's live client (checked non-null above); this
    // reads its device pointer.
    let dev = unsafe { (*file).dev };
    // SAFETY: `lock_ptr`'s contract: `dev` is the client's live device.
    let lock = unsafe { lock_ptr(dev) };
    // SAFETY: `lock` is the device's lock word; the pair brackets the handle insert.
    unsafe { irq_spinlock_acquire(lock) };
    {
        // SAFETY: `file` is the live client; this borrow is consumed by the handle
        // insert below.
        let file = unsafe { &mut *file };
        file.next_handle += 1;
        let handle = file.next_handle;
        file.handles.insert(handle, obj);
        drm_gem_ref(obj);
        // SAFETY: as above — releasing the same lock.
        unsafe { irq_spinlock_release(lock) };
        // SAFETY: `handle_out` is the caller's out-parameter for the handle.
        unsafe { *handle_out = handle };
    }
    0
}

#[no_mangle]
pub extern "C" fn drm_gem_handle_lookup(file: *mut DrmFile, handle: u32) -> *mut GemObject {
    if file.is_null() || handle == 0 {
        return core::ptr::null_mut();
    }
    // SAFETY: read-only walk of the caller's file.
    unsafe {
        match (*file).handles.get(&handle) {
            Some(&obj) => obj,
            None => core::ptr::null_mut(),
        }
    }
}

/// Release a handle, dropping the object reference it held.
#[no_mangle]
pub extern "C" fn drm_gem_handle_close(file: *mut DrmFile, handle: u32) -> c_int {
    if file.is_null() {
        return -1;
    }
    // SAFETY: caller's file.
    unsafe {
        match (*file).handles.remove(&handle) {
            Some(obj) => {
                drm_gem_unref(obj);
                0
            }
            None => -9, // -EBADF
        }
    }
}

/* ── PRIME ──────────────────────────────────────────────────────────────── */

/// Export a GEM object as a file descriptor.  Because the storage is a memfd,
/// this wraps that same memfd in a vnode — no copy, and both sides see one set
/// of frames.
#[no_mangle]
pub extern "C" fn drm_gem_prime_handle_to_fd(
    dev: *mut DrmDevice,
    file: *mut DrmFile,
    handle: u32,
    flags: u32,
    fd_out: *mut c_int,
) -> c_int {
    let _ = flags;
    if dev.is_null() || file.is_null() || fd_out.is_null() {
        return -22;
    }
    // SAFETY: `drm_gem_handle_lookup`'s contract: this only walks the caller's file.
    let obj = drm_gem_handle_lookup(file, handle);
    if obj.is_null() {
        return -9; // -EBADF
    }

    // SAFETY: `obj` is the live object named by the handle; this reads its memfd.
    let memfd = unsafe { (*obj).memfd };
    // SAFETY: as above — its size.
    let size = unsafe { (*obj).size };

    // SAFETY: `memfd` is that live object's backing memfd and `size` its length.
    let node = unsafe {
        memfd_vnode_from_handle(memfd, c"drm-prime".as_ptr() as *const u8, size)
    };
    if node.is_null() {
        return -12;
    }

    // SAFETY: `node` is the live vnode just created; `alloc_fd`'s contract wants a
    // live node to install as the new fd's file.
    let fd = unsafe { alloc_fd(node) };
    if fd < 0 {
        return -24; // -EMFILE
    }

    // SAFETY: `fd_out` is the caller's out-parameter for the fd.
    unsafe { *fd_out = fd };
    0
}

/// Import: find or create the object that owns the memfd behind `fd`.  A handle
/// exported by this very device resolves to the existing object; an fd from
/// elsewhere (another device, or a plain memfd) is adopted as a new object
/// sharing the same storage.
#[no_mangle]
pub extern "C" fn drm_gem_prime_fd_to_handle(
    dev: *mut DrmDevice,
    file: *mut DrmFile,
    fd: c_int,
    handle_out: *mut u32,
) -> c_int {
    if dev.is_null() || file.is_null() || handle_out.is_null() {
        return -22;
    }
    // SAFETY: `memfd_fd_handle` is a kernel C service mapping an fd to its memfd.
    let h = unsafe { memfd_fd_handle(fd) };
    if h <= 0 {
        return -22;
    }

    let mut obj: *mut GemObject = core::ptr::null_mut();
    // SAFETY: `lock_ptr`'s contract: `dev` is the caller's live device.
    let lock = unsafe { lock_ptr(dev) };
    // SAFETY: `lock` is the device's lock word; the pair brackets the object-list
    // walk.
    unsafe { irq_spinlock_acquire(lock) };
    // SAFETY: `dev` is the live device; this borrow of its object list is consumed
    // by the walk below.
    let gem_list = unsafe { &(*dev).gem_list };
    for &o in gem_list.iter() {
        if !o.is_null() {
            // SAFETY: `o` is a live object (non-null, checked above).
            if unsafe { (*o).memfd } == h {
                obj = o;
                break;
            }
        }
    }
    // SAFETY: as above — releasing the same lock.
    unsafe { irq_spinlock_release(lock) };

    if !obj.is_null() {
        let mut handle = 0u32;
        if drm_gem_handle_create(file, obj, &mut handle) != 0 {
            return -12;
        }
        // SAFETY: `handle_out` is the caller's out-parameter for the handle.
        unsafe { *handle_out = handle };
        return 0;
    }

    // SAFETY: `memfd_size` is a kernel C service reading the memfd's length.
    let size = unsafe { memfd_size(h) };
    if size <= 0 {
        return -22;
    }

    let obj = cact_mm::kmalloc(core::mem::size_of::<GemObject>() as u32) as *mut GemObject;
    if obj.is_null() {
        return -12;
    }
    // SAFETY: `obj` is the fresh `kmalloc` object just checked non-null, so zeroing
    // one `GemObject` covers exactly that allocation.
    unsafe { core::ptr::write_bytes(obj, 0, 1) };
    {
        // SAFETY: `obj` is the fresh, exclusively-owned object; this borrow is
        // consumed by the field stores below.
        let obj_ref = unsafe { &mut *obj };
        obj_ref.dev = dev;
        obj_ref.memfd = h;
        obj_ref.size = size as u32;
        obj_ref.refcount = 1;
    }
    // SAFETY: `h` is the live memfd being adopted; the object now owns a reference
    // of its own.
    unsafe { memfd_ref(h) };

    // SAFETY: `dev` is the live device; this copies its ops pointer.
    let ops = unsafe { (*dev).ops };
    if !ops.is_null() {
        // SAFETY: `ops` is the driver's live ops table; this copies the `gem_create`
        // fn pointer only.
        let create = unsafe { (*ops).gem_create };
        if let Some(create) = create {
            // `create` is the plain `extern "C"` driver hook and `dev`/`obj` are
            // the live device and object it expects.
            if create(dev, obj) != 0 {
                // SAFETY: on refusal the adopted memfd and the object are still ours.
                unsafe { memfd_close(h) };
                // SAFETY: as above.
                unsafe { cact_mm::kfree(obj as *mut u8) };
                return -22;
            }
        }
    }

    // SAFETY: `lock` is the device's lock word; the pair brackets the link update.
    unsafe { irq_spinlock_acquire(lock) };
    drm_gem_link(dev, obj);
    // SAFETY: as above — releasing the same lock.
    unsafe { irq_spinlock_release(lock) };

    let mut handle = 0u32;
    if drm_gem_handle_create(file, obj, &mut handle) != 0 {
        drm_gem_unref(obj);
        return -12;
    }
    // SAFETY: `handle_out` is the caller's out-parameter for the handle.
    unsafe { *handle_out = handle };
    0
}

/* ── dumb buffers ───────────────────────────────────────────────────────── */

/* uapi/drm_mode.h, sizes pinned by the probe that filled these assertions. */
#[repr(C)]
struct ModeCreateDumb {
    height: u32,
    width: u32,
    bpp: u32,
    flags: u32,
    handle: u32,
    pitch: u32,
    size: u64,
}

#[repr(C)]
struct ModeMapDumb {
    handle: u32,
    pad: u32,
    offset: u64,
}

#[repr(C)]
struct ModeDestroyDumb {
    handle: u32,
}

const _: () = assert!(core::mem::size_of::<ModeCreateDumb>() == 32);
const _: () = assert!(core::mem::offset_of!(ModeCreateDumb, size) == 24);
const _: () = assert!(core::mem::size_of::<ModeMapDumb>() == 16);
const _: () = assert!(core::mem::offset_of!(ModeMapDumb, offset) == 8);
const _: () = assert!(core::mem::size_of::<ModeDestroyDumb>() == 4);

/// `_IOWR(0x64, nr, size)` — the ioctl numbers, spelled out so the payload
/// check in `drm_copy_in` sees exactly the size these structs have.
const fn drm_iowr(nr: u32, size: u32) -> u32 {
    (3u32 << 30) | (0x64u32 << 8) | nr | (size << 16)
}

const DRM_IOCTL_MODE_CREATE_DUMB: u32 = drm_iowr(0xB2, 32);
const DRM_IOCTL_MODE_MAP_DUMB: u32 = drm_iowr(0xB3, 16);
const DRM_IOCTL_MODE_DESTROY_DUMB: u32 = drm_iowr(0xB4, 4);

/// `DRM_IOCTL_MODE_CREATE_DUMB`.  Takes the *userspace* pointer and copies its
/// arguments in and the result back out — the same discipline every handler
/// follows, so a client cannot make the kernel write to an address of its
/// choosing.
#[no_mangle]
pub extern "C" fn drm_gem_dumb_create(file: *mut DrmFile, user: *mut c_void) -> c_int {
    if file.is_null() {
        return -22;
    }
    let mut args = ModeCreateDumb {
        height: 0,
        width: 0,
        bpp: 0,
        flags: 0,
        handle: 0,
        pitch: 0,
        size: 0,
    };
    if drm_copy_in(
        &mut args as *mut _ as *mut c_void,
        DRM_IOCTL_MODE_CREATE_DUMB,
        user,
        core::mem::size_of::<ModeCreateDumb>() as u32,
    ) != 0
    {
        return -22;
    }
    if args.width == 0 || args.height == 0 {
        return -22;
    }

    let bpp = if args.bpp != 0 { args.bpp } else { 32 };
    if bpp != 8 && bpp != 16 && bpp != 24 && bpp != 32 {
        return -22;
    }

    /* Scanout pitch: keep rows 64-byte aligned so a blitter or the device's
     * own refresh stays on cache-line boundaries. */
    let pitch = (args.width * (bpp / 8) + 63) & !63u32;
    if pitch == 0 {
        return -22;
    }
    if args.height > 0xFFFF_FFFFu32 / pitch {
        return -22;
    }
    let size = pitch * args.height;
    if size == 0 {
        return -22;
    }

    // SAFETY: `file` is the caller's live client (checked non-null above); this
    // reads its device pointer.
    let dev = unsafe { (*file).dev };
    // SAFETY: `drm_gem_create`'s contract: `dev` is the live device; it returns a
    // fresh object with one reference held.
    let obj = drm_gem_create(dev, size);
    if obj.is_null() {
        return -12;
    }

    {
        // SAFETY: `obj` is the fresh object created above and exclusively owned here;
        // this borrow is consumed by the field stores.
        let obj_ref = unsafe { &mut *obj };
        obj_ref.is_dumb = 1;
        obj_ref.width = args.width;
        obj_ref.height = args.height;
        obj_ref.bpp = bpp;
        obj_ref.pitch = pitch;
    }

    let mut off = 0u64;
    if drm_gem_map_offset(dev, obj, &mut off) != 0 {
        drm_gem_unref(obj);
        return -12;
    }
    // SAFETY: static NUL-terminated string.
    unsafe { printk(c"  drm         : DBG dumb map_offset ok\n".as_ptr() as *const u8) };

    let mut handle = 0u32;
    if drm_gem_handle_create(file, obj, &mut handle) != 0 {
        drm_gem_unref(obj);
        return -12;
    }

    /* No zeroing here: memfd frames are zeroed as they are allocated, and a linear
     * memset over the whole object would run off the first frame anyway — the
     * frames are individually allocated. */

    args.handle = handle;
    args.pitch = pitch;
    args.size = size as u64;
    drm_copy_out(user, &args as *const _ as *const c_void, core::mem::size_of::<ModeCreateDumb>() as u32)
}

#[no_mangle]
pub extern "C" fn drm_gem_dumb_map_offset(file: *mut DrmFile, user: *mut c_void) -> c_int {
    if file.is_null() {
        return -22;
    }
    let mut args = ModeMapDumb { handle: 0, pad: 0, offset: 0 };
    if drm_copy_in(
        &mut args as *mut _ as *mut c_void,
        DRM_IOCTL_MODE_MAP_DUMB,
        user,
        core::mem::size_of::<ModeMapDumb>() as u32,
    ) != 0
    {
        return -22;
    }

    // SAFETY: `drm_gem_handle_lookup`'s contract: this only walks the caller's file.
    let obj = drm_gem_handle_lookup(file, args.handle);
    if obj.is_null() {
        return -9;
    }
    // SAFETY: `obj` is the live object named by the handle; this reads its dumb flag.
    if unsafe { (*obj).is_dumb } == 0 {
        return -22;
    }
    // SAFETY: as above — the object's cached map offset.
    args.offset = unsafe { (*obj).map_offset } as u64;
    drm_copy_out(user, &args as *const _ as *const c_void, core::mem::size_of::<ModeMapDumb>() as u32)
}

#[no_mangle]
pub extern "C" fn drm_gem_dumb_destroy(file: *mut DrmFile, user: *mut c_void) -> c_int {
    if file.is_null() {
        return -22;
    }
    let mut args = ModeDestroyDumb { handle: 0 };
    if drm_copy_in(
        &mut args as *mut _ as *mut c_void,
        DRM_IOCTL_MODE_DESTROY_DUMB,
        user,
        core::mem::size_of::<ModeDestroyDumb>() as u32,
    ) != 0
    {
        return -22;
    }
    drm_gem_handle_close(file, args.handle)
}
