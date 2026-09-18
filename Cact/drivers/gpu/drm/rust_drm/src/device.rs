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

use crate::ffi::printk;
use crate::gem::drm_gem_unref;
use crate::kms::framebuffer::{ClipRect, Framebuffer};
use crate::kms::mode_object::{Connector, Crtc, Plane};
use crate::structs::{DrmDevice, DriverOps, IrqSpinlock, ModeSet};

extern "C" {
    fn kfree(ptr: *mut c_void);
    fn irq_spinlock_init(lock: *mut IrqSpinlock);

    /// VFS/devfs glue: publishing `/dev/dri` and the card/render nodes.
    fn drm_devfs_init() -> c_int;
    fn drm_devfs_add_device(dev: *mut DrmDevice) -> c_int;
}

/// Every registered device, indexed by minor.  `None` marks a free minor.
static mut DEVICES: Option<Vec<Option<*mut DrmDevice>>> = None;

// SAFETY: device allocation/removal happens while the module loader holds the
// kernel's module lock, which is also how the C table was protected.
unsafe fn devices() -> &'static mut Vec<Option<*mut DrmDevice>> {
    let p = core::ptr::addr_of_mut!(DEVICES);
    if (*p).is_none() {
        *p = Some(Vec::new());
    }
    (*p).as_mut().unwrap()
}

/// The device table for the devfs glue to walk.
pub(crate) unsafe fn device_table() -> &'static mut Vec<Option<*mut DrmDevice>> {
    devices()
}

/* ── lifecycle ──────────────────────────────────────────────────────────── */

/// Allocate a DRM device.  `priv` is the driver's own per-device state and is
/// returned by `drm_dev_priv()`.  NULL on out-of-memory.
#[no_mangle]
pub extern "C" fn drm_dev_alloc(ops: *const DriverOps, priv_: *mut c_void) -> *mut DrmDevice {
    if ops.is_null() {
        return core::ptr::null_mut();
    }
    // SAFETY: registers a freshly allocated device in the global table.
    unsafe {
        let table = devices();
        let minor = match table.iter().position(|e| e.is_none()) {
            Some(i) => i,
            None => table.len(),
        };

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
        irq_spinlock_init(core::ptr::addr_of_mut!((*dev).lock));

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
    // SAFETY: caller's device.
    unsafe {
        drm_dev_unregister(dev);
        let minor = (*dev).minor as usize;
        let table = devices();
        if minor < table.len() {
            table[minor] = None;
        }
        drop(Box::from_raw(dev));
    }
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
    // SAFETY: caller's device; the driver's load() is expected to be callable
    // with it, as in C.
    unsafe {
        if (*dev).ops.is_null() {
            return -1;
        }

        /* Idempotent: the first device registers /dev/dri, later ones reuse it. */
        if drm_devfs_init() != 0 {
            return -1;
        }

        /* Create the device node first: a driver's load() often pushes its
         * initial mode through the KMS state, and clients may already open the
         * node. */
        if drm_devfs_add_device(dev) != 0 {
            printk(
                b"\x013  drm         : cannot publish /dev/dri/card%d\n\0".as_ptr(),
                (*dev).minor,
            );
            return -1;
        }

        let ops = (*dev).ops;
        if let Some(load) = (*ops).load {
            let rc = load(dev);
            if rc != 0 {
                printk(
                    b"\x013  drm         : '%s' load failed (%d)\n\0".as_ptr(),
                    (*ops).name,
                    rc,
                );
                return rc;
            }
        }

        (*dev).in_use = 1;

        let count = |pool: &[*mut c_void]| pool.iter().filter(|&&p| !p.is_null()).count() as u32;
        let ncrtc = count(&(*dev).crtcs);
        let nconn = count(&(*dev).connectors);
        let nenc = count(&(*dev).encoders);
        let nplane = count(&(*dev).planes);
        let nfb = count(&(*dev).fbs);

        printk(
            b"\x016  %-11s : /dev/dri/card%d ready \xe2\x80\x94 %s (%u crtc, %u conn, %u enc, %u plane, %u fb)\n\0"
                .as_ptr(),
            b"drm\0".as_ptr(),
            (*dev).minor,
            (*ops).name,
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
    // SAFETY: caller's device; every pool entry is checked for emptiness.
    unsafe {
        if (*dev).in_use == 0 {
            return;
        }

        /* Drop every client: their handles and framebuffers hold object
         * references, so this must happen before the driver tears its hardware
         * down. */
        for &f in (*dev).clients.iter() {
            if !f.is_null() {
                crate::file::drm_client_destroy(f);
            }
        }
        (*dev).clients.clear();

        let ops = (*dev).ops;
        if !ops.is_null() {
            if let Some(unload) = (*ops).unload {
                unload(dev);
            }
        }

        /* CRTCs and encoders are plain kalloc'd structs … */
        for &p in (*dev).crtcs.iter() {
            if !p.is_null() {
                kfree(p);
            }
        }
        (*dev).crtcs.clear();
        for &p in (*dev).encoders.iter() {
            if !p.is_null() {
                kfree(p);
            }
        }
        (*dev).encoders.clear();

        /* … while connectors and planes own `Vec`s, so they are dropped. */
        for &p in (*dev).connectors.iter() {
            if !p.is_null() {
                drop(Box::from_raw(p as *mut Connector));
            }
        }
        (*dev).connectors.clear();
        for &p in (*dev).planes.iter() {
            if !p.is_null() {
                drop(Box::from_raw(p as *mut Plane));
            }
        }
        (*dev).planes.clear();

        for &p in (*dev).fbs.iter() {
            let fb = p as *mut Framebuffer;
            if !fb.is_null() {
                if !(*fb).obj.is_null() {
                    drm_gem_unref((*fb).obj);
                }
                kfree(fb as *mut c_void);
            }
        }
        (*dev).fbs.clear();

        /* Vec-typed pools release themselves once cleared. */
        (*dev).props.clear();
        (*dev).prop_attach.clear();
        (*dev).blobs.clear();
        (*dev).flink.clear();
        (*dev).map_offsets.clear();

        /* The /dev/dri nodes are ours (drm_devfs_add_device allocated them). */
        kfree((*dev).card_node as *mut c_void);
        (*dev).card_node = core::ptr::null_mut();
        kfree((*dev).render_node as *mut c_void);
        (*dev).render_node = core::ptr::null_mut();
        (*dev).have_render_node = 0;

        (*dev).in_use = 0;
    }
}

/* ── driver-ops call-throughs ───────────────────────────────────────────── */

#[no_mangle]
pub extern "C" fn drm_driver_set_config(dev: *mut DrmDevice, set: *mut ModeSet) -> c_int {
    if dev.is_null() {
        return 0;
    }
    // SAFETY: caller's device; the ops table is the driver's.
    unsafe {
        let ops = (*dev).ops;
        if ops.is_null() {
            return 0;
        }
        match (*ops).set_config {
            Some(f) => f(dev, set),
            None => 0,
        }
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
    // SAFETY: caller's device and framebuffer.
    unsafe {
        let ops = (*dev).ops;
        if ops.is_null() {
            return 0;
        }
        match (*ops).dirty {
            Some(f) => f(fb as *mut c_void, clips as *const c_void, num_clips),
            None => 0,
        }
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
    // SAFETY: caller's device, CRTC and framebuffer.
    unsafe {
        let ops = (*dev).ops;
        if ops.is_null() {
            return 0;
        }
        match (*ops).page_flip {
            Some(f) => f(crtc as *mut c_void, fb as *mut c_void, flags, user_data),
            None => 0,
        }
    }
}

/// A driver without a vblank interrupt leaves `enable_vblank` NULL; 0 then means
/// "nothing to do", not "failed".
#[no_mangle]
pub extern "C" fn drm_driver_enable_vblank(dev: *mut DrmDevice, crtc: *mut Crtc) -> c_int {
    if dev.is_null() {
        return 0;
    }
    // SAFETY: caller's device and CRTC.
    unsafe {
        let ops = (*dev).ops;
        if ops.is_null() {
            return 0;
        }
        match (*ops).enable_vblank {
            Some(f) => f(dev, crtc as *mut c_void),
            None => 0,
        }
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
