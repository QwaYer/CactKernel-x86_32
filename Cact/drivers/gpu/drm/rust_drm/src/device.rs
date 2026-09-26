//! Device lifecycle: allocation, registration (which publishes `/dev/dri/cardN`
//! and calls the driver's `load()`), unregistration, and the global device
//! table.
//!
//! The device table is a `Vec`, so there is no fixed maximum number of DRM
//! devices; a device itself is a boxed Rust value, so its pools are released
//! when it is freed.

use alloc::boxed::Box;
use alloc::collections::BTreeMap;
use alloc::vec::Vec;
use core::ffi::{c_int, c_void};

use crate::devfs::{drm_devfs_add_device, drm_devfs_init};
use crate::ffi::printk;
use crate::gem::drm_gem_unref;
use crate::kms::framebuffer::{ClipRect, Framebuffer};
use crate::kms::mode_object::{Connector, Crtc, Plane};
use crate::structs::{DrmDevice, DriverOps, IrqSpinlock, ModeSet};

extern "C" {

    fn irq_spinlock_init(lock: *mut IrqSpinlock);
}

/// Every registered device, indexed by minor.  `None` marks a free minor.
static mut DEVICES: Option<Vec<Option<*mut DrmDevice>>> = None;

// SAFETY: device allocation/removal happens while the module loader holds the
// kernel's module lock, which is also how the C table was protected.
unsafe fn devices() -> &'static mut Vec<Option<*mut DrmDevice>> {
    let p = core::ptr::addr_of_mut!(DEVICES);
    // SAFETY: `p` is the address of this crate's own `DEVICES` static, which
    // outlives every device; the module lock serialises callers.
    if unsafe { (*p).is_none() } {
        // SAFETY: as above — installing the initial `Vec` exactly once.
        unsafe { *p = Some(Vec::new()) };
    }
    // SAFETY: as above — `DEVICES` is now initialised, so this borrow is valid for
    // as long as the caller uses it (the module lock keeps it unaliased).
    unsafe { (*p).as_mut().unwrap() }
}

/// The device table for the devfs glue to walk.
pub(crate) unsafe fn device_table() -> &'static mut Vec<Option<*mut DrmDevice>> {
    // SAFETY: forwards to `devices()`, so the same module-lock invariant holds.
    unsafe { devices() }
}

/* ── lifecycle ──────────────────────────────────────────────────────────── */

/// Allocate a DRM device.  `priv` is the driver's own per-device state and is
/// returned by `drm_dev_priv()`.  NULL on out-of-memory.
#[no_mangle]
pub extern "C" fn drm_dev_alloc(ops: *const DriverOps, priv_: *mut c_void) -> *mut DrmDevice {
    if ops.is_null() {
        return core::ptr::null_mut();
    }
    // SAFETY: `devices()` returns the module-locked device table, so the slot search
    // and the store below are serialised.
    let table = unsafe { devices() };
    let minor = match table.iter().position(|e| e.is_none()) {
        Some(i) => i,
        None => table.len(),
    };

    {

        let dev = Box::new(DrmDevice {
            ops,
            priv_,
            minor: minor as i32,
            in_use: 0,
            next_magic: 0x4000_0000u32.wrapping_add((minor as u32).wrapping_mul(0x1000)),
            next_fb_id: 0,
            next_prop_id: 0,
            next_blob_id: 0,
            lock: IrqSpinlock::new(),
            crtcs: Vec::new(),
            connectors: Vec::new(),
            encoders: Vec::new(),
            planes: Vec::new(),
            fbs: Vec::new(),
            props: Vec::new(),
            prop_attach: Vec::new(),
            blobs: Vec::new(),
            map_offsets: BTreeMap::new(),
            next_map_offset: 0x1000_0000u32
                .wrapping_add((minor as u32).wrapping_mul(0x1000_0000)),
            gem_list: Vec::new(),
            flink: BTreeMap::new(),
            next_flink_name: 1,
            clients: Vec::new(),
            syncobjs: Vec::new(),
            next_syncobj_id: 0,
            sync_fds: Vec::new(),
            card_node: core::ptr::null_mut(),
            render_node: core::ptr::null_mut(),
            have_render_node: 0,
        });
        let dev = Box::into_raw(dev);
        // SAFETY: `addr_of_mut!` forms a pointer to the fresh device's `lock` field
        // (the device is not published yet) without creating a reference.
        let lock = unsafe { core::ptr::addr_of_mut!((*dev).lock) };
        // SAFETY: `lock` is that field and the device is exclusively owned here.
        unsafe { irq_spinlock_init(lock) };

        if table.len() <= minor {
            table.push(Some(dev));
        } else {
            table[minor] = Some(dev);
        }
        dev
    }
}

/// Tear a device down completely and release its slot.
#[no_mangle]
pub extern "C" fn drm_dev_free(dev: *mut DrmDevice) {
    if dev.is_null() {
        return;
    }
    drm_dev_unregister(dev);
    // SAFETY: `dev` is the caller's live device (checked non-null above); this reads
    // its slot index.
    let minor = unsafe { (*dev).minor } as usize;
    // SAFETY: `devices()` returns the module-locked device table.
    let table = unsafe { devices() };
    if minor < table.len() {
        table[minor] = None;
    }
    // SAFETY: `dev` came from `Box::into_raw` in `drm_dev_alloc` and has just been
    // removed from the table, so reclaiming the Box here is the matching free.
    unsafe { drop(Box::from_raw(dev)) };
}

#[no_mangle]
pub extern "C" fn drm_dev_priv(dev: *mut DrmDevice) -> *mut c_void {
    if dev.is_null() {
        core::ptr::null_mut()
    } else {
        // SAFETY: caller's device.
        unsafe { (*dev).priv_ }
    }
}

#[no_mangle]
pub extern "C" fn drm_dev_ops(dev: *mut DrmDevice) -> *const DriverOps {
    if dev.is_null() {
        core::ptr::null()
    } else {
        // SAFETY: caller's device.
        unsafe { (*dev).ops }
    }
}

/// Bring the device up: call `ops->load()`, then publish `/dev/dri/cardN` (and
/// the matching `renderDN`).  Returns 0 on success, negative on failure — a
/// driver that fails here must call `drm_dev_free()`.
#[no_mangle]
pub extern "C" fn drm_dev_register(dev: *mut DrmDevice) -> c_int {
    if dev.is_null() {
        return -1;
    }
    // SAFETY: `dev` is the caller's live device (checked non-null above); this reads
    // its ops pointer.
    let ops = unsafe { (*dev).ops };
    if ops.is_null() {
        return -1;
    }

    /* Idempotent: the first device registers /dev/dri, later ones reuse it. */
    if drm_devfs_init() != 0 {
        return -1;
    }

    /* Create the device node first: a driver's load() often pushes its initial
     * mode through the KMS state, and clients may already open the node. */
    if drm_devfs_add_device(dev) != 0 {
        // SAFETY: `dev` is the live device; this reads its minor for the message.
        let minor = unsafe { (*dev).minor };
        // SAFETY: `printk` takes a NUL-terminated format string plus its arguments.
        unsafe {
            printk(
                c"\x013  drm         : cannot publish /dev/dri/card%d\n".as_ptr() as *const u8,
                minor,
            );
        }
        return -1;
    }

    // SAFETY: `ops` is the driver's live ops table; this copies the `load` fn
    // pointer only (a plain `extern "C"` pointer, so the call needs no block).
    let load = unsafe { (*ops).load };
    if let Some(load) = load {
        let rc = load(dev);
        if rc != 0 {
            // SAFETY: `ops` is the driver's live ops table; this reads its name.
            let name = unsafe { (*ops).name };
            // SAFETY: `printk` takes a NUL-terminated format string plus arguments.
            unsafe {
                printk(
                    c"\x013  drm         : '%s' load failed (%d)\n".as_ptr() as *const u8,
                    name,
                    rc,
                );
            }
            return rc;
        }
    }

    // SAFETY: `dev` is the live device; marking it in use.
    unsafe { (*dev).in_use = 1 };

    let count = |pool: &[*mut c_void]| pool.iter().filter(|&&p| !p.is_null()).count() as u32;
    // SAFETY: `dev` is the live device; this borrow of its CRTC pool is consumed by
    // the count.
    let ncrtc = count(unsafe { &(*dev).crtcs });
    // SAFETY: as above — connectors.
    let nconn = count(unsafe { &(*dev).connectors });
    // SAFETY: as above — encoders.
    let nenc = count(unsafe { &(*dev).encoders });
    // SAFETY: as above — planes.
    let nplane = count(unsafe { &(*dev).planes });
    // SAFETY: as above — framebuffers.
    let nfb = count(unsafe { &(*dev).fbs });

    // SAFETY: `dev` is the live device; this reads its minor for the banner.
    let minor = unsafe { (*dev).minor };
    // SAFETY: `ops` is the driver's live ops table; this reads its name.
    let name = unsafe { (*ops).name };
    // SAFETY: `printk` takes a NUL-terminated format string plus its arguments.
    unsafe {
        printk(
            c"\x016  %-11s : /dev/dri/card%d ready \u{2014} %s (%u crtc, %u conn, %u enc, %u plane, %u fb)\n"
                .as_ptr() as *const u8,
            c"drm".as_ptr() as *const u8,
            minor,
            name,
            ncrtc,
            nconn,
            nenc,
            nplane,
            nfb,
        );
    }
    0
}

/// Tear the device down.  Clients and their handles go first, then the driver,
/// then the core's own pools.
#[no_mangle]
pub extern "C" fn drm_dev_unregister(dev: *mut DrmDevice) {
    if dev.is_null() {
        return;
    }
    // SAFETY: `dev` is the caller's live device (checked non-null above); this reads
    // its in-use flag.
    if unsafe { (*dev).in_use } == 0 {
        return;
    }

    /* Drop every client: their handles and framebuffers hold object references, so
     * this must happen before the driver tears its hardware down. */
    // SAFETY: `dev` is the live device; this borrow of its client list is consumed by
    // the drain.
    let clients = unsafe { &(*dev).clients };
    for &f in clients.iter() {
        if !f.is_null() {
            // SAFETY: `drm_client_destroy`'s contract: `f` is one of this device's
            // clients.
            unsafe { crate::file::drm_client_destroy(f) };
        }
    }
    // SAFETY: as above — clearing the (now-empty) client list.
    unsafe { (*dev).clients.clear() };

    // SAFETY: `dev` is the live device; this copies its ops pointer.
    let ops = unsafe { (*dev).ops };
    if !ops.is_null() {
        // SAFETY: `ops` is the driver's live ops table; this copies the `unload` fn
        // pointer only (a plain `extern "C"` pointer).
        let unload = unsafe { (*ops).unload };
        if let Some(unload) = unload {
            unload(dev);
        }
    }

    /* CRTCs and encoders are plain kalloc'd structs … */
    // SAFETY: `dev` is the live device; this borrow of its CRTC list is consumed by
    // the drain.
    let crtcs = unsafe { &(*dev).crtcs };
    for &p in crtcs.iter() {
        if !p.is_null() {
            // SAFETY: each entry is a `kalloc`'d CRTC this device owns.
            unsafe { cact_mm::kfree(p as *mut u8) };
        }
    }
    // SAFETY: as above — clearing the CRTC list.
    unsafe { (*dev).crtcs.clear() };
    // SAFETY: as above — the encoder list.
    let encoders = unsafe { &(*dev).encoders };
    for &p in encoders.iter() {
        if !p.is_null() {
            // SAFETY: each entry is a `kalloc`'d encoder this device owns.
            unsafe { cact_mm::kfree(p as *mut u8) };
        }
    }
    // SAFETY: as above — clearing the encoder list.
    unsafe { (*dev).encoders.clear() };

    /* … while connectors and planes own `Vec`s, so they are dropped. */
    // SAFETY: `dev` is the live device; this borrow of its connector list is consumed
    // by the drain.
    let connectors = unsafe { &(*dev).connectors };
    for &p in connectors.iter() {
        if !p.is_null() {
            // SAFETY: each entry came from `Box::into_raw` and is owned by this device.
            unsafe { drop(Box::from_raw(p as *mut Connector)) };
        }
    }
    // SAFETY: as above — clearing the connector list.
    unsafe { (*dev).connectors.clear() };
    // SAFETY: as above — the plane list.
    let planes = unsafe { &(*dev).planes };
    for &p in planes.iter() {
        if !p.is_null() {
            // SAFETY: each entry came from `Box::into_raw` and is owned by this device.
            unsafe { drop(Box::from_raw(p as *mut Plane)) };
        }
    }
    // SAFETY: as above — clearing the plane list.
    unsafe { (*dev).planes.clear() };

    // SAFETY: `dev` is the live device; this borrow of its framebuffer list is
    // consumed by the drain.
    let fbs = unsafe { &(*dev).fbs };
    for &p in fbs.iter() {
        let fb = p as *mut Framebuffer;
        if !fb.is_null() {
            // SAFETY: `fb` is a live framebuffer; this reads its GEM object.
            let obj = unsafe { (*fb).obj };
            if !obj.is_null() {
                drm_gem_unref(obj);
            }
            // SAFETY: `fb` is a `kalloc`'d framebuffer this device owns.
            unsafe { cact_mm::kfree(fb as *mut u8) };
        }
    }
    // SAFETY: as above — clearing the framebuffer list.
    unsafe { (*dev).fbs.clear() };

    /* Vec-typed pools release themselves once cleared. */
    // SAFETY: `dev` is the live device; clearing each remaining pool.
    unsafe { (*dev).props.clear() };
    // SAFETY: as above — the property-attachment table.
    unsafe { (*dev).prop_attach.clear() };
    // SAFETY: as above — the blob table.
    unsafe { (*dev).blobs.clear() };
    // SAFETY: as above — the global-name table.
    unsafe { (*dev).flink.clear() };
    // SAFETY: as above — the map-offset table.
    unsafe { (*dev).map_offsets.clear() };

    /* The /dev/dri nodes are ours (drm_devfs_add_device allocated them). */
    // SAFETY: `dev` is the live device; this reads its card-node pointer.
    let card_node = unsafe { (*dev).card_node };
    // SAFETY: the card node was allocated by `kmalloc` for this device.
    unsafe { cact_mm::kfree(card_node as *mut u8) };
    // SAFETY: as above — clearing the pointer after the free.
    unsafe { (*dev).card_node = core::ptr::null_mut() };
    // SAFETY: `dev` is the live device; this reads its render-node pointer.
    let render_node = unsafe { (*dev).render_node };
    // SAFETY: the render node was allocated by `kmalloc` for this device.
    unsafe { cact_mm::kfree(render_node as *mut u8) };
    // SAFETY: as above — clearing the pointer after the free.
    unsafe { (*dev).render_node = core::ptr::null_mut() };
    // SAFETY: as above — clearing the has-render-node flag.
    unsafe { (*dev).have_render_node = 0 };

    // SAFETY: as above — marking the device slot free.
    unsafe { (*dev).in_use = 0 };
}

/* ── driver-ops call-throughs ───────────────────────────────────────────── */

#[no_mangle]
pub extern "C" fn drm_driver_set_config(dev: *mut DrmDevice, set: *mut ModeSet) -> c_int {
    if dev.is_null() {
        return 0;
    }
    // SAFETY: `dev` is the caller's live device; this copies its ops pointer.
    let ops = unsafe { (*dev).ops };
    if ops.is_null() {
        return 0;
    }
    // SAFETY: `ops` is the driver's live ops table; this copies the `set_config` fn
    // pointer only (a plain `extern "C"` pointer).
    let f = unsafe { (*ops).set_config };
    match f {
        Some(f) => f(dev, set),
        None => 0,
    }
}

#[no_mangle]
pub extern "C" fn drm_driver_dirty(
    dev: *mut DrmDevice,
    fb: *mut Framebuffer,
    clips: *const ClipRect,
    num_clips: u32,
) -> c_int {
    if dev.is_null() {
        return 0;
    }
    // SAFETY: `dev` is the caller's live device; this copies its ops pointer.
    let ops = unsafe { (*dev).ops };
    if ops.is_null() {
        return 0;
    }
    // SAFETY: `ops` is the driver's live ops table; this copies the `dirty` fn
    // pointer only (a plain `extern "C"` pointer).
    let f = unsafe { (*ops).dirty };
    match f {
        Some(f) => f(fb as *mut c_void, clips as *const c_void, num_clips),
        None => 0,
    }
}

#[no_mangle]
pub extern "C" fn drm_driver_page_flip(
    dev: *mut DrmDevice,
    crtc: *mut Crtc,
    fb: *mut Framebuffer,
    flags: u32,
    user_data: *mut c_void,
) -> c_int {
    if dev.is_null() {
        return 0;
    }
    // SAFETY: `dev` is the caller's live device; this copies its ops pointer.
    let ops = unsafe { (*dev).ops };
    if ops.is_null() {
        return 0;
    }
    // SAFETY: `ops` is the driver's live ops table; this copies the `page_flip` fn
    // pointer only (a plain `extern "C"` pointer).
    let f = unsafe { (*ops).page_flip };
    match f {
        Some(f) => f(crtc as *mut c_void, fb as *mut c_void, flags, user_data),
        None => 0,
    }
}

/// A driver without a vblank interrupt leaves `enable_vblank` NULL; 0 then means
/// "nothing to do", not "failed".
#[no_mangle]
pub extern "C" fn drm_driver_enable_vblank(dev: *mut DrmDevice, crtc: *mut Crtc) -> c_int {
    if dev.is_null() {
        return 0;
    }
    // SAFETY: `dev` is the caller's live device; this copies its ops pointer.
    let ops = unsafe { (*dev).ops };
    if ops.is_null() {
        return 0;
    }
    // SAFETY: `ops` is the driver's live ops table; this copies the `enable_vblank`
    // fn pointer only (a plain `extern "C"` pointer).
    let f = unsafe { (*ops).enable_vblank };
    match f {
        Some(f) => f(dev, crtc as *mut c_void),
        None => 0,
    }
}

/// Convenience for module probes: alloc + register in one step.
#[no_mangle]
pub extern "C" fn drm_dev_create(ops: *const DriverOps, priv_: *mut c_void) -> *mut DrmDevice {
    let dev = drm_dev_alloc(ops, priv_);
    if dev.is_null() {
        return core::ptr::null_mut();
    }
    if drm_dev_register(dev) != 0 {
        drm_dev_free(dev);
        return core::ptr::null_mut();
    }
    dev
}
