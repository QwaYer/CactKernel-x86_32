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
    if validate_user_ptr(user, bytes) == 0 {
        return false;
    }
    copy_from_user(out.as_mut_ptr() as *mut c_void, user, bytes) == 0
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
    if validate_user_ptr(user, bytes) == 0 {
        return false;
    }
    copy_from_user(out.as_mut_ptr() as *mut c_void, user, bytes) == 0
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
                    let blob = (*dev).find_blob(value as u32);
                    if blob.is_null() {
                        return -22;
                    }
                    let blob = &*blob;
                    if blob.data.len() != core::mem::size_of::<ModeInfo>() {
                        return -22;
                    }
                    let mi = &*(blob.data.as_ptr() as *const ModeInfo);
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
                        if !(*plane).formats.iter().any(|&f| f == (*fb).format) {
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

    /// Program the driver once and publish the new state.
    unsafe fn commit(&mut self, dev: *mut DrmDevice, flags: u32, user_data: u64) -> c_int {
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
        let mut fb = (*crtc).fb as *mut Framebuffer;
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
        if let Some(m) = self.mode {
            let cur = (*crtc).mode;
            let same = m.clock == cur.clock
                && m.hdisplay == cur.hdisplay
                && m.vdisplay == cur.vdisplay
                && m.htotal == cur.htotal
                && m.vtotal == cur.vtotal;
            if !same && (*crtc).enabled != 0 && flags & DRM_MODE_ATOMIC_ALLOW_MODESET == 0 {
                return -22;
            }
        }

        let mode = self.mode.unwrap_or((*crtc).mode);
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
        if fb_changed && (*crtc).fb != fb as *mut c_void {
            if !(*crtc).fb.is_null() {
                drm_gem_unref((*((*crtc).fb as *mut Framebuffer)).obj);
            }
            (*crtc).fb = fb as *mut c_void;
            if !fb.is_null() {
                drm_gem_ref((*fb).obj);
            }
        }
        match self.active {
            Some(a) => (*crtc).enabled = if a != 0 { 1 } else { 0 },
            None => {
                if !fb.is_null() {
                    (*crtc).enabled = 1;
                }
            }
        }
        if let Some(m) = self.mode {
            (*crtc).mode = m;
        }

        /* Connector binding. */
        if !self.conn.is_null() {
            if let Some(cid) = self.conn_crtc_id {
                if cid == 0 {
                    if (*crtc).connector == self.conn as *mut c_void {
                        (*crtc).connector = core::ptr::null_mut();
                    }
                    (*self.conn).encoder = core::ptr::null_mut();
                } else {
                    if (*self.conn).encoder.is_null() {
                        let bit = 1u32 << (*crtc).index;
                        for &e in (*dev).encoders.iter() {
                            let enc = e as *mut Encoder;
                            if !enc.is_null() && (*enc).possible_crtcs & bit != 0 {
                                drm_connector_attach_encoder(self.conn, e);
                                break;
                            }
                        }
                    }
                    if !(*self.conn).encoder.is_null() {
                        drm_encoder_attach_crtc(
                            (*self.conn).encoder as *mut Encoder,
                            crtc as *mut c_void,
                        );
                        (*crtc).connector = self.conn as *mut c_void;
                    }
                }
            }
        }

        /* Plane state, so GETPLANE reports what the commit installed. */
        if !self.plane.is_null() {
            if let Some(v) = self.plane_crtc_id {
                (*self.plane).crtc_id = v;
            }
            if let Some(v) = self.plane_fb_id {
                (*self.plane).fb_id = v;
            }
            let dst = [4usize, 5, 6, 7];
            if let Some(v) = self.rects[0] {
                (*self.plane).src_x = v;
            }
            if let Some(v) = self.rects[1] {
                (*self.plane).src_y = v;
            }
            if let Some(v) = self.rects[2] {
                (*self.plane).src_w = v;
            }
            if let Some(v) = self.rects[3] {
                (*self.plane).src_h = v;
            }
            if let Some(v) = self.rects[dst[0]] {
                (*self.plane).crtc_x = v as i32;
            }
            if let Some(v) = self.rects[dst[1]] {
                (*self.plane).crtc_y = v as i32;
            }
            if let Some(v) = self.rects[dst[2]] {
                (*self.plane).crtc_w = v;
            }
            if let Some(v) = self.rects[dst[3]] {
                (*self.plane).crtc_h = v;
            }
        }

        if flags & DRM_MODE_PAGE_FLIP_EVENT != 0 && fb_changed {
            drm_crtc_vblank_bump(crtc);
            drm_file_queue_event(
                dev,
                crtc,
                DRM_EVENT_FLIP_COMPLETE,
                user_data,
                (*crtc).vblank_count,
            );
        }
        0
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

    // SAFETY: caller's file; every userspace array is range-checked before use.
    unsafe {
        let dev = (*file).dev;
        if dev.is_null() {
            return -22;
        }

        let mut objs: Vec<u32> = vec![0u32; n];
        let mut counts: Vec<u32> = vec![0u32; n];
        if !read_u32s(req.objs_ptr, &mut objs) || !read_u32s(req.count_props_ptr, &mut counts) {
            return -14; // -EFAULT
        }
        let total: usize = counts.iter().map(|&c| c as usize).sum();
        if total == 0 || total > MAX_ATOMIC_PROPS {
            return -22;
        }
        let mut props: Vec<u32> = vec![0u32; total];
        let mut vals: Vec<u64> = vec![0u64; total];
        if !read_u32s(req.props_ptr, &mut props) || !read_u64s(req.prop_values_ptr, &mut vals) {
            return -14;
        }

        let mut st = AtomicState::new();
        let mut idx = 0usize;
        for i in 0..n {
            for _ in 0..counts[i] as usize {
                let rc = st.apply(dev, objs[i], props[idx], vals[idx]);
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
        st.commit(dev, req.flags, req.user_data)
    }
}
