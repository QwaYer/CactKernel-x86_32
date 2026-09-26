//! The atomic modeset API: `DRM_IOCTL_MODE_ATOMIC`.
//!
//! A client hands over a list of objects, and for each of them a list of
//! `(property, value)` pairs.  The properties are the ones this core defines
//! (`ACTIVE`, `MODE_ID`, `CRTC_ID`, `FB_ID`, the plane rectangles and `type`),
//! which is why the property knows its object type: that is how a bare object
//! id in the request is told apart between the CRTC, connector and plane pools.
//!
//! `DRM_MODE_ATOMIC_TEST_ONLY` runs every check and returns without touching
//! anything — that is what a compositor uses to probe a configuration — and a
//! real commit programs the driver once, through the same `set_config` the
//! legacy SETCRTC path uses (on this device the primary plane *is* the scanout).

use alloc::vec;
use alloc::vec::Vec;
use core::ffi::{c_int, c_void};

use super::framebuffer::{drm_fb_find, Framebuffer};
use super::mode_object::{
    drm_connector_attach_encoder, drm_connector_find, drm_crtc_find, drm_encoder_attach_crtc,
    drm_plane_find, Connector, Crtc, Encoder, Plane,
};
use super::property::drm_prop_name_and_type;
use crate::device::drm_driver_set_config;
use crate::event::{drm_crtc_vblank_bump, drm_file_queue_event};
use crate::ffi::drm_copy_in;
use crate::gem::{drm_gem_ref, drm_gem_unref};
use crate::mode::{drm_modeinfo_to_mode, DisplayMode, ModeInfo, MODE_NAME_LEN};
use crate::structs::{DrmDevice, DrmFile, ModeSet};

/* ── uapi (uapi/drm_mode.h) ─────────────────────────────────────────────── */

/// `struct drm_mode_atomic` — 56 bytes.
#[repr(C)]
pub struct ModeAtomic {
    pub flags: u32,
    pub count_objs: u32,
    pub objs_ptr: u64,
    pub count_props_ptr: u64,
    pub props_ptr: u64,
    pub prop_values_ptr: u64,
    pub reserved: u64,
    pub user_data: u64,
}

const _: () = assert!(core::mem::size_of::<ModeAtomic>() == 56);
const _: () = assert!(core::mem::offset_of!(ModeAtomic, objs_ptr) == 8);
const _: () = assert!(core::mem::offset_of!(ModeAtomic, count_props_ptr) == 16);
const _: () = assert!(core::mem::offset_of!(ModeAtomic, user_data) == 48);

const DRM_IOCTL_MODE_ATOMIC: u32 = (3u32 << 30) | (0x64u32 << 8) | 0xBC | (56 << 16);

const DRM_MODE_ATOMIC_TEST_ONLY: u32 = 0x0100;
const DRM_MODE_ATOMIC_NONBLOCK: u32 = 0x0200;
const DRM_MODE_ATOMIC_ALLOW_MODESET: u32 = 0x0400;
const DRM_MODE_PAGE_FLIP_EVENT: u32 = 0x01;
const DRM_MODE_ATOMIC_FLAGS: u32 = DRM_MODE_PAGE_FLIP_EVENT
    | DRM_MODE_ATOMIC_TEST_ONLY
    | DRM_MODE_ATOMIC_NONBLOCK
    | DRM_MODE_ATOMIC_ALLOW_MODESET;

const DRM_MODE_OBJECT_CRTC: u32 = 0xcccc_cccc;
const DRM_MODE_OBJECT_CONNECTOR: u32 = 0xc0c0_c0c0;
const DRM_MODE_OBJECT_PLANE: u32 = 0xeeee_eeee;
const DRM_EVENT_FLIP_COMPLETE: u32 = 0x02;

/* One commit may not name more objects or properties than this: a compositor
 * uses a handful, and both arrays come straight from userspace. */
const MAX_ATOMIC_OBJS: usize = 32;
const MAX_ATOMIC_PROPS: usize = 256;

extern "C" {
    fn validate_user_ptr(ptr: *const c_void, size: u32) -> c_int;
    fn copy_from_user(dst: *mut c_void, src: *const c_void, size: u32) -> c_int;
}

/// Copy a `u32` array in from userspace; false on a bad pointer.
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

/// Copy a `u64` array in from userspace; false on a bad pointer.
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

fn zero_mode() -> DisplayMode {
    DisplayMode {
        clock: 0,
        hdisplay: 0,
        hsync_start: 0,
        hsync_end: 0,
        htotal: 0,
        hskew: 0,
        vdisplay: 0,
        vsync_start: 0,
        vsync_end: 0,
        vtotal: 0,
        vscan: 0,
        flags: 0,
        mtype: 0,
        name: [0; MODE_NAME_LEN],
    }
}

/* ── the staged state ───────────────────────────────────────────────────── */

/// What one commit asks for.  Only the objects the client actually named end
/// up here, so `None` means "leave it as it is".
struct AtomicState {
    crtc: *mut Crtc,
    conn: *mut Connector,
    plane: *mut Plane,
    mode: Option<DisplayMode>,
    active: Option<u32>,
    conn_crtc_id: Option<u32>,
    plane_crtc_id: Option<u32>,
    plane_fb_id: Option<u32>,
    /// `SRC_X, SRC_Y, SRC_W, SRC_H, CRTC_X, CRTC_Y, CRTC_W, CRTC_H`
    rects: [Option<u32>; 8],
}

impl AtomicState {
    fn new() -> Self {
        AtomicState {
            crtc: core::ptr::null_mut(),
            conn: core::ptr::null_mut(),
            plane: core::ptr::null_mut(),
            mode: None,
            active: None,
            conn_crtc_id: None,
            plane_crtc_id: None,
            plane_fb_id: None,
            rects: [None; 8],
        }
    }

    /// Validate and stage one `(object, property, value)` triple.
    unsafe fn apply(
        &mut self,
        dev: *mut DrmDevice,
        obj_id: u32,
        prop_id: u32,
        value: u64,
    ) -> c_int {
        // `dev` is the caller's live device and every id in the request is resolved
        // through its own pools or a checked `find_*` lookup, so each object pointer
        // staged here names a live object of that device.
        {
            if obj_id == 0 || prop_id == 0 {
                return -22;
            }
            let (name, ptype) = drm_prop_name_and_type(dev, prop_id);
            if ptype == 0 {
                return -22; // no such property
            }
            let is = |want: &[u8]| name[..want.len()] == *want;

            match ptype {
                DRM_MODE_OBJECT_CRTC => {
                    let crtc = drm_crtc_find(dev, obj_id) as *mut Crtc;
                    if crtc.is_null() {
                        return -22;
                    }
                    self.crtc = crtc;
                    if is(b"ACTIVE") {
                        if value > 1 {
                            return -22;
                        }
                        self.active = Some(value as u32);
                        0
                    } else if is(b"MODE_ID") {
                        if value == 0 {
                            self.mode = None;
                            return 0;
                        }
                        // SAFETY: `dev` is the caller's live device; this finds the blob
                        // by id.
                        let blob = unsafe { (*dev).find_blob(value as u32) };
                        if blob.is_null() {
                            return -22;
                        }
                        // SAFETY: `blob` is the live blob (non-null, checked above); this
                        // borrow is consumed by the reads below.
                        let blob = unsafe { &*blob };
                        if blob.data.len() != core::mem::size_of::<ModeInfo>() {
                            return -22;
                        }
                        // SAFETY: the length check above proved the payload is a whole
                        // `ModeInfo`, so this typed read is in bounds and aligned.
                        let mi = unsafe { &*(blob.data.as_ptr() as *const ModeInfo) };
                        let mut m = zero_mode();
                        drm_modeinfo_to_mode(mi, &mut m);
                        m.name[MODE_NAME_LEN - 1] = 0;
                        self.mode = Some(m);
                        0
                    } else {
                        -22
                    }
                }
                DRM_MODE_OBJECT_CONNECTOR => {
                    let conn = drm_connector_find(dev, obj_id) as *mut Connector;
                    if conn.is_null() {
                        return -22;
                    }
                    self.conn = conn;
                    if is(b"CRTC_ID") {
                        if value != 0 && (drm_crtc_find(dev, value as u32) as *mut Crtc).is_null() {
                            return -22;
                        }
                        self.conn_crtc_id = Some(value as u32);
                        0
                    } else {
                        -22
                    }
                }
                DRM_MODE_OBJECT_PLANE => {
                    let plane = drm_plane_find(dev, obj_id) as *mut Plane;
                    if plane.is_null() {
                        return -22;
                    }
                    self.plane = plane;
                    if is(b"CRTC_ID") {
                        if value != 0 && (drm_crtc_find(dev, value as u32) as *mut Crtc).is_null() {
                            return -22;
                        }
                        self.plane_crtc_id = Some(value as u32);
                        0
                    } else if is(b"FB_ID") {
                        if value != 0 {
                            let fb = drm_fb_find(dev, value as u32) as *mut Framebuffer;
                            if fb.is_null() {
                                return -22;
                            }
                            // SAFETY: `plane` is the live plane and `fb` the live
                            // framebuffer found above; this borrow of the plane's format
                            // list is consumed by the scan.
                            let formats = unsafe { &(*plane).formats };
                            // SAFETY: as above — the framebuffer's pixel format.
                            let fb_format = unsafe { (*fb).format };
                            if !formats.contains(&fb_format) {
                                return -22;
                            }
                        }
                        self.plane_fb_id = Some(value as u32);
                        0
                    } else {
                        let slot = match &name[..] {
                            _ if is(b"SRC_X") => 0,
                            _ if is(b"SRC_Y") => 1,
                            _ if is(b"SRC_W") => 2,
                            _ if is(b"SRC_H") => 3,
                            _ if is(b"CRTC_X") => 4,
                            _ if is(b"CRTC_Y") => 5,
                            _ if is(b"CRTC_W") => 6,
                            _ if is(b"CRTC_H") => 7,
                            _ => return -22,
                        };
                        self.rects[slot] = Some(value as u32);
                        0
                    }
                }
                _ => -22,
            }
        }
    }

    /// Program the driver once and publish the new state.
    unsafe fn commit(&mut self, dev: *mut DrmDevice, flags: u32, user_data: u64) -> c_int {
        /* `dev` is the caller's live device, `crtc`/`conn`/`plane` were each resolved
         * from its own pool by `apply`, and every framebuffer is looked up in the
         * device's list, so all pointers here name live objects. */
        {
            /* The CRTC comes from the object the client named, or from the plane
             * it attached. */
            let mut crtc = self.crtc;
            if crtc.is_null() {
                if let Some(id) = self.plane_crtc_id {
                    crtc = drm_crtc_find(dev, id) as *mut Crtc;
                }
            }
            if crtc.is_null() {
                return -22;
            }

            /* The framebuffer: a plane's FB_ID is what a compositor drives. */
            // SAFETY: `crtc` is a live CRTC (checked non-null above); this reads its
            // framebuffer.
            let mut fb = unsafe { (*crtc).fb } as *mut Framebuffer;
            let mut fb_changed = false;
            if let Some(fbid) = self.plane_fb_id {
                fb_changed = true;
                fb = if fbid == 0 {
                    core::ptr::null_mut()
                } else {
                    drm_fb_find(dev, fbid) as *mut Framebuffer
                };
                if fbid != 0 && fb.is_null() {
                    return -22;
                }
            }
            let turning_off = matches!(self.active, Some(0));
            if turning_off {
                fb = core::ptr::null_mut();
                fb_changed = true;
            }

            /* Changing the mode of an enabled CRTC is a modeset: it needs
             * ALLOW_MODESET, exactly as Linux insists. */
            // SAFETY: `crtc` is a live CRTC; this reads its current mode.
            let cur = unsafe { (*crtc).mode };
            // SAFETY: as above — its enabled flag.
            let crtc_enabled = unsafe { (*crtc).enabled };
            if let Some(m) = self.mode {
                let same = m.clock == cur.clock
                    && m.hdisplay == cur.hdisplay
                    && m.vdisplay == cur.vdisplay
                    && m.htotal == cur.htotal
                    && m.vtotal == cur.vtotal;
                if !same && crtc_enabled != 0 && flags & DRM_MODE_ATOMIC_ALLOW_MODESET == 0 {
                    return -22;
                }
            }

            let mode = self.mode.unwrap_or(cur);
            /* Fill the connector list before taking its pointer: the driver reads
             * it through `set.connectors`. */
            let mut conns: [*mut c_void; 1] = [core::ptr::null_mut()];
            let mut num_connectors = 0i32;
            if !self.conn.is_null() {
                conns[0] = self.conn as *mut c_void;
                num_connectors = 1;
            }
            let mut set = ModeSet {
                fb: fb as *mut c_void,
                crtc: crtc as *mut c_void,
                mode,
                x: self.rects[4].map(|v| v as i32).unwrap_or(0),
                y: self.rects[5].map(|v| v as i32).unwrap_or(0),
                connectors: conns.as_mut_ptr(),
                num_connectors,
            };

            let rc = drm_driver_set_config(dev, &mut set);
            if rc != 0 {
                return rc;
            }

            /* Publish only after the driver accepted the state. */
            // SAFETY: `crtc` is a live CRTC; this reads its framebuffer.
            let cur_fb = unsafe { (*crtc).fb };
            if fb_changed && cur_fb != fb as *mut c_void {
                if !cur_fb.is_null() {
                    // SAFETY: `cur_fb` is a live framebuffer; this reads its GEM object.
                    let obj = unsafe { (*(cur_fb as *mut Framebuffer)).obj };
                    drm_gem_unref(obj);
                }
                // SAFETY: as above — installing the new framebuffer.
                unsafe { (*crtc).fb = fb as *mut c_void };
                if !fb.is_null() {
                    // SAFETY: `fb` is the live framebuffer; this reads its GEM object to
                    // take a reference.
                    let obj = unsafe { (*fb).obj };
                    drm_gem_ref(obj);
                }
            }
            match self.active {
                // SAFETY: `crtc` is a live CRTC; this stores its new active state.
                Some(a) => unsafe { (*crtc).enabled = if a != 0 { 1 } else { 0 } },
                None => {
                    if !fb.is_null() {
                        // SAFETY: as above — an attached framebuffer means enabled.
                        unsafe { (*crtc).enabled = 1 };
                    }
                }
            }
            if let Some(m) = self.mode {
                // SAFETY: as above — installing the new mode.
                unsafe { (*crtc).mode = m };
            }

            /* Connector binding. */
            if !self.conn.is_null() {
                if let Some(cid) = self.conn_crtc_id {
                    if cid == 0 {
                        // SAFETY: `crtc` is the live CRTC and `self.conn` the staged live
                        // connector; this detaches them.
                        let attached = unsafe { (*crtc).connector };
                        if attached == self.conn as *mut c_void {
                            // SAFETY: as above — clearing the CRTC's connector.
                            unsafe { (*crtc).connector = core::ptr::null_mut() };
                        }
                        // SAFETY: `self.conn` is the staged live connector; this clears
                        // its encoder.
                        unsafe { (*self.conn).encoder = core::ptr::null_mut() };
                    } else {
                        // SAFETY: `self.conn` is the staged live connector; this reads its
                        // encoder.
                        let conn_enc = unsafe { (*self.conn).encoder };
                        if conn_enc.is_null() {
                            /* Pick any encoder that can drive this CRTC. */
                            // SAFETY: `crtc` is the live CRTC; this reads its index.
                            let bit = 1u32 << unsafe { (*crtc).index };
                            // SAFETY: `dev` is the live device; this borrow of its encoder
                            // pool is consumed by the scan.
                            let encoders = unsafe { &(*dev).encoders };
                            for &e in encoders.iter() {
                                let enc = e as *mut Encoder;
                                // SAFETY: `enc` is a live encoder (non-null, checked above);
                                // this reads its CRTC mask.
                                if !enc.is_null() && unsafe { (*enc).possible_crtcs } & bit != 0 {
                                    drm_connector_attach_encoder(self.conn, e);
                                    break;
                                }
                            }
                        }
                        // SAFETY: `self.conn` is the live connector; this re-reads its
                        // encoder.
                        let conn_enc = unsafe { (*self.conn).encoder };
                        if !conn_enc.is_null() {
                            drm_encoder_attach_crtc(conn_enc as *mut Encoder, crtc as *mut c_void);
                            // SAFETY: as above — recording the CRTC's connector.
                            unsafe { (*crtc).connector = self.conn as *mut c_void };
                        }
                    }
                }
            }

            /* Plane state, so GETPLANE reports what the commit installed. */
            if !self.plane.is_null() {
                if let Some(v) = self.plane_crtc_id {
                    // SAFETY: `self.plane` is the staged live plane; this stores its CRTC
                    // id.
                    unsafe { (*self.plane).crtc_id = v };
                }
                if let Some(v) = self.plane_fb_id {
                    // SAFETY: as above — its framebuffer id.
                    unsafe { (*self.plane).fb_id = v };
                }
                let dst = [4usize, 5, 6, 7];
                if let Some(v) = self.rects[0] {
                    // SAFETY: as above — its source rectangle.
                    unsafe { (*self.plane).src_x = v };
                }
                if let Some(v) = self.rects[1] {
                    // SAFETY: as above — its source rectangle.
                    unsafe { (*self.plane).src_y = v };
                }
                if let Some(v) = self.rects[2] {
                    // SAFETY: as above — its source rectangle.
                    unsafe { (*self.plane).src_w = v };
                }
                if let Some(v) = self.rects[3] {
                    // SAFETY: as above — its source rectangle.
                    unsafe { (*self.plane).src_h = v };
                }
                if let Some(v) = self.rects[dst[0]] {
                    // SAFETY: as above — its destination rectangle.
                    unsafe { (*self.plane).crtc_x = v as i32 };
                }
                if let Some(v) = self.rects[dst[1]] {
                    // SAFETY: as above — its destination rectangle.
                    unsafe { (*self.plane).crtc_y = v as i32 };
                }
                if let Some(v) = self.rects[dst[2]] {
                    // SAFETY: as above — its destination rectangle.
                    unsafe { (*self.plane).crtc_w = v };
                }
                if let Some(v) = self.rects[dst[3]] {
                    // SAFETY: as above — its destination rectangle.
                    unsafe { (*self.plane).crtc_h = v };
                }
            }

            if flags & DRM_MODE_PAGE_FLIP_EVENT != 0 && fb_changed {
                drm_crtc_vblank_bump(crtc);
                // SAFETY: `crtc` is the live CRTC; this reads its vblank sequence.
                let seq = unsafe { (*crtc).vblank_count };
                drm_file_queue_event(dev, crtc, DRM_EVENT_FLIP_COMPLETE, user_data, seq);
            }
            0
        }
    }
}

/* ── the ioctl ──────────────────────────────────────────────────────────── */

#[no_mangle]
pub extern "C" fn mode_atomic(file: *mut DrmFile, arg: *mut c_void) -> c_int {
    if file.is_null() {
        return -22;
    }
    let mut req = ModeAtomic {
        flags: 0,
        count_objs: 0,
        objs_ptr: 0,
        count_props_ptr: 0,
        props_ptr: 0,
        prop_values_ptr: 0,
        reserved: 0,
        user_data: 0,
    };
    if drm_copy_in(
        &mut req as *mut _ as *mut c_void,
        DRM_IOCTL_MODE_ATOMIC,
        arg,
        core::mem::size_of::<ModeAtomic>() as u32,
    ) != 0
    {
        return -22;
    }
    if req.flags & !DRM_MODE_ATOMIC_FLAGS != 0 {
        return -22;
    }
    let n = req.count_objs as usize;
    if n == 0 || n > MAX_ATOMIC_OBJS {
        return -22;
    }

    // SAFETY: `file` is the caller's live client (checked non-null above); this reads
    // its device pointer.
    let dev = unsafe { (*file).dev };
    if dev.is_null() {
        return -22;
    }

    let mut objs: Vec<u32> = vec![0u32; n];
    let mut counts: Vec<u32> = vec![0u32; n];
    // SAFETY: `read_u32s`'s contract: `req.objs_ptr` is the user array and `objs` has
    // exactly `n` elements.
    if !unsafe { read_u32s(req.objs_ptr, &mut objs) } {
        return -14; // -EFAULT
    }
    // SAFETY: `read_u32s`'s contract: `req.count_props_ptr` is the user array and
    // `counts` has exactly `n` elements.
    if !unsafe { read_u32s(req.count_props_ptr, &mut counts) } {
        return -14; // -EFAULT
    }
    let total: usize = counts.iter().map(|&c| c as usize).sum();
    if total == 0 || total > MAX_ATOMIC_PROPS {
        return -22;
    }
    let mut props: Vec<u32> = vec![0u32; total];
    let mut vals: Vec<u64> = vec![0u64; total];
    // SAFETY: `read_u32s`'s contract: `req.props_ptr` is the user array and `props`
    // has exactly `total` elements.
    if !unsafe { read_u32s(req.props_ptr, &mut props) } {
        return -14;
    }
    // SAFETY: `read_u64s`'s contract: `req.prop_values_ptr` is the user array and
    // `vals` has exactly `total` elements.
    if !unsafe { read_u64s(req.prop_values_ptr, &mut vals) } {
        return -14;
    }

    let mut st = AtomicState::new();
    let mut idx = 0usize;
    for i in 0..n {
        for _ in 0..counts[i] as usize {
            // SAFETY: `apply`'s contract: `dev` is the caller's live device and every
            // id is resolved through its own pools.
            let rc = unsafe { st.apply(dev, objs[i], props[idx], vals[idx]) };
            if rc != 0 {
                return rc;
            }
            idx += 1;
        }
    }

    /* TEST_ONLY stops here: everything above was validation. */
    if req.flags & DRM_MODE_ATOMIC_TEST_ONLY != 0 {
        return 0;
    }
    // SAFETY: `commit`'s contract: `dev` is the caller's live device and the staged
    // state names live objects.
    unsafe { st.commit(dev, req.flags, req.user_data) }
}
