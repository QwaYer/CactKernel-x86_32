//! The CRTC ioctls: GETCRTC, SETCRTC (the non-atomic modeset, including
//! connector/encoder resolution) and PAGE_FLIP.
//!
//! The driver is always programmed *before* the new state is published to
//! userspace: if `set_config` refuses the mode, nothing changes.

use alloc::vec::Vec;
use core::ffi::{c_int, c_void};

use super::framebuffer::{drm_fb_find, Framebuffer};
use super::mode_object::{
    drm_connector_attach_encoder, drm_connector_find, drm_crtc_find, drm_encoder_attach_crtc,
    Connector, Crtc, Encoder,
};
use crate::device::{drm_driver_enable_vblank, drm_driver_page_flip, drm_driver_set_config};
use crate::event::{drm_crtc_vblank_bump, drm_file_queue_event};
use crate::ffi::{drm_copy_in, drm_copy_out};
use crate::mode::{drm_mode_to_modeinfo, drm_modeinfo_to_mode, DisplayMode, ModeInfo, MODE_NAME_LEN};
use crate::structs::{DrmDevice, DrmFile, GemObject, ModeSet};

/* ── uapi structs (uapi/drm_mode.h) ─────────────────────────────────────── */

/// `struct drm_mode_crtc` (GETCRTC / SETCRTC) — 104 bytes.
#[repr(C)]
pub struct ModeCrtc {
    pub set_connectors_ptr: u64,
    pub count_connectors: u32,
    pub crtc_id: u32,
    pub fb_id: u32,
    pub x: u32,
    pub y: u32,
    pub gamma_size: u32,
    pub mode_valid: u32,
    pub mode: ModeInfo,
}

/// `struct drm_mode_crtc_page_flip` — 24 bytes.
#[repr(C)]
pub struct ModeCrtcPageFlip {
    pub crtc_id: u32,
    pub fb_id: u32,
    pub flags: u32,
    pub reserved: u32,
    pub user_data: u64,
}

const _: () = assert!(core::mem::size_of::<ModeCrtc>() == 104);
const _: () = assert!(core::mem::offset_of!(ModeCrtc, crtc_id) == 12);
const _: () = assert!(core::mem::offset_of!(ModeCrtc, fb_id) == 16);
const _: () = assert!(core::mem::offset_of!(ModeCrtc, mode_valid) == 32);
const _: () = assert!(core::mem::offset_of!(ModeCrtc, mode) == 36);
const _: () = assert!(core::mem::size_of::<ModeCrtcPageFlip>() == 24);
const _: () = assert!(core::mem::offset_of!(ModeCrtcPageFlip, fb_id) == 4);
const _: () = assert!(core::mem::offset_of!(ModeCrtcPageFlip, flags) == 8);
const _: () = assert!(core::mem::offset_of!(ModeCrtcPageFlip, user_data) == 16);

const fn drm_iowr(nr: u32, size: u32) -> u32 {
    (3u32 << 30) | (0x64u32 << 8) | nr | (size << 16)
}
const DRM_IOCTL_MODE_GETCRTC: u32 = drm_iowr(0xA1, 104);
const DRM_IOCTL_MODE_SETCRTC: u32 = drm_iowr(0xA2, 104);
const DRM_IOCTL_MODE_PAGE_FLIP: u32 = drm_iowr(0xB0, 24);

const DRM_MODE_PAGE_FLIP_EVENT: u32 = 0x01;
const DRM_EVENT_FLIP_COMPLETE: u32 = 0x02;

/* One SETCRTC request may name this many connectors; a compositor uses one or
 * two, and the array comes straight from userspace, so the copy is bounded
 * rather than unbounded. */
const MAX_SET_CONNECTORS: usize = 64;

extern "C" {
    fn validate_user_ptr(ptr: *const c_void, size: u32) -> c_int;
    fn copy_from_user(dst: *mut c_void, src: *const c_void, size: u32) -> c_int;
    fn drm_gem_ref(obj: *mut GemObject);
    fn drm_gem_unref(obj: *mut GemObject);
    fn drm_gem_handle_lookup(file: *mut DrmFile, handle: u32) -> *mut GemObject;
}

fn empty_mode() -> ModeInfo {
    ModeInfo {
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
        vrefresh: 0,
        flags: 0,
        mtype: 0,
        name: [0; MODE_NAME_LEN],
    }
}

fn zero_display_mode() -> DisplayMode {
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

fn empty_set() -> ModeSet {
    ModeSet {
        fb: core::ptr::null_mut(),
        crtc: core::ptr::null_mut(),
        mode: zero_display_mode(),
        x: 0,
        y: 0,
        connectors: core::ptr::null_mut(),
        num_connectors: 0,
    }
}

/* ── GETCRTC ────────────────────────────────────────────────────────────── */

#[no_mangle]
pub extern "C" fn mode_get_crtc(file: *mut DrmFile, arg: *mut c_void) -> c_int {
    if file.is_null() {
        return -22;
    }
    let mut req = ModeCrtc {
        set_connectors_ptr: 0,
        count_connectors: 0,
        crtc_id: 0,
        fb_id: 0,
        x: 0,
        y: 0,
        gamma_size: 0,
        mode_valid: 0,
        mode: empty_mode(),
    };
    if drm_copy_in(
        &mut req as *mut _ as *mut c_void,
        DRM_IOCTL_MODE_GETCRTC,
        arg,
        core::mem::size_of::<ModeCrtc>() as u32,
    ) != 0
    {
        return -22;
    }

    // SAFETY: caller's file; the CRTC is looked up in its device.
    let c = unsafe { drm_crtc_find((*file).dev, req.crtc_id) } as *mut Crtc;
    if c.is_null() {
        return -22;
    }

    // SAFETY: looked up above; the mode is copied out by value.
    unsafe {
        let mut mi = empty_mode();
        drm_mode_to_modeinfo(&(*c).mode, &mut mi);

        req.set_connectors_ptr = 0;
        req.count_connectors = 0;
        req.fb_id = if (*c).fb.is_null() {
            0
        } else {
            (*((*c).fb as *mut Framebuffer)).id
        };
        req.x = (*c).x as u32;
        req.y = (*c).y as u32;
        req.gamma_size = 0;
        req.mode_valid = if (*c).enabled != 0 { 1 } else { 0 };
        /* drm_mode_get_crtc carries the mode inline, not by pointer. */
        req.mode = mi;
    }

    drm_copy_out(
        arg,
        &req as *const _ as *const c_void,
        core::mem::size_of::<ModeCrtc>() as u32,
    )
}

/* ── SETCRTC ────────────────────────────────────────────────────────────── */

#[no_mangle]
pub extern "C" fn mode_set_crtc(file: *mut DrmFile, arg: *mut c_void) -> c_int {
    if file.is_null() {
        return -22;
    }
    let mut req = ModeCrtc {
        set_connectors_ptr: 0,
        count_connectors: 0,
        crtc_id: 0,
        fb_id: 0,
        x: 0,
        y: 0,
        gamma_size: 0,
        mode_valid: 0,
        mode: empty_mode(),
    };
    if drm_copy_in(
        &mut req as *mut _ as *mut c_void,
        DRM_IOCTL_MODE_SETCRTC,
        arg,
        core::mem::size_of::<ModeCrtc>() as u32,
    ) != 0
    {
        return -22;
    }

    // SAFETY: caller's file; objects are looked up in its device.
    unsafe {
        let dev = (*file).dev;
        let crtc = drm_crtc_find(dev, req.crtc_id) as *mut Crtc;
        if crtc.is_null() {
            return -22;
        }

        /* fb_id == 0 turns the CRTC off, which is how a compositor releases the
         * display (drmModeSetCrtc(fd, crtc, 0, 0, 0, NULL, 0)). */
        if req.fb_id == 0 {
            let mut set = empty_set();
            set.crtc = crtc as *mut c_void;
            set.mode = (*crtc).mode;
            let rc = drm_driver_set_config(dev, &mut set);
            if rc != 0 {
                return rc;
            }
            if !(*crtc).fb.is_null() {
                drm_gem_unref((*((*crtc).fb as *mut Framebuffer)).obj);
            }
            (*crtc).fb = core::ptr::null_mut();
            (*crtc).enabled = 0;
            (*crtc).connector = core::ptr::null_mut();
            return 0;
        }

        let fb = drm_fb_find(dev, req.fb_id) as *mut Framebuffer;
        if fb.is_null() {
            return -22;
        }

        let mut set = empty_set();
        set.crtc = crtc as *mut c_void;
        set.fb = fb as *mut c_void;
        set.x = req.x as i32;
        set.y = req.y as i32;
        drm_modeinfo_to_mode(&req.mode, &mut set.mode);
        set.mode.name[MODE_NAME_LEN - 1] = 0;

        /* The connector list is rebuilt here (it lives no longer than the call)
         * and is where the encoder/CRTC binding is established on the legacy
         * path. */
        let mut conns: Vec<*mut c_void> = Vec::new();
        if req.count_connectors != 0 && req.set_connectors_ptr != 0 {
            let count = req.count_connectors as usize;
            let n = if count > MAX_SET_CONNECTORS {
                MAX_SET_CONNECTORS
            } else {
                count
            };
            let mut ids: Vec<u32> = Vec::new();
            ids.resize(n, 0);
            let user = req.set_connectors_ptr as u32 as usize as *const c_void;
            let bytes = (n * 4) as u32;
            if validate_user_ptr(user, bytes) == 0 {
                return -22;
            }
            if copy_from_user(ids.as_mut_ptr() as *mut c_void, user, bytes) != 0 {
                return -22;
            }

            for &id in ids.iter() {
                let c = drm_connector_find(dev, id) as *mut Connector;
                if c.is_null() {
                    return -22;
                }
                conns.push(c as *mut c_void);

                if (*c).encoder.is_null() {
                    /* Pick any encoder that can drive this CRTC. */
                    let bit = 1u32 << (*crtc).index;
                    for &e in (*dev).encoders.iter() {
                        let enc = e as *mut Encoder;
                        if enc.is_null() {
                            continue;
                        }
                        if (*enc).possible_crtcs & bit != 0 {
                            drm_connector_attach_encoder(c, e);
                            break;
                        }
                    }
                }
                if !(*c).encoder.is_null() {
                    drm_encoder_attach_crtc((*c).encoder as *mut Encoder, crtc as *mut c_void);
                }
            }
        }
        if conns.is_empty() {
            return -22;
        }
        set.connectors = conns.as_mut_ptr();
        set.num_connectors = conns.len() as i32;

        /* The driver programs the hardware first: only when it accepts the mode
         * do we publish the new state to userspace. */
        let rc = drm_driver_set_config(dev, &mut set);
        if rc != 0 {
            return rc;
        }

        if !(*crtc).fb.is_null() && (*crtc).fb != fb as *mut c_void {
            drm_gem_unref((*((*crtc).fb as *mut Framebuffer)).obj);
        }
        (*crtc).fb = fb as *mut c_void;
        (*crtc).mode = set.mode;
        (*crtc).x = set.x;
        (*crtc).y = set.y;
        (*crtc).enabled = 1;
        (*crtc).connector = conns[0];

        drm_gem_ref((*fb).obj);
        drm_crtc_vblank_bump(crtc);
        if (*crtc).vblank_enabled == 0 && drm_driver_enable_vblank(dev, crtc) == 0 {
            (*crtc).vblank_enabled = 1;
        }
    }
    0
}

/* ── PAGE_FLIP ──────────────────────────────────────────────────────────── */

#[no_mangle]
pub extern "C" fn mode_page_flip(file: *mut DrmFile, arg: *mut c_void) -> c_int {
    if file.is_null() {
        return -22;
    }
    let mut req = ModeCrtcPageFlip {
        crtc_id: 0,
        fb_id: 0,
        flags: 0,
        reserved: 0,
        user_data: 0,
    };
    if drm_copy_in(
        &mut req as *mut _ as *mut c_void,
        DRM_IOCTL_MODE_PAGE_FLIP,
        arg,
        core::mem::size_of::<ModeCrtcPageFlip>() as u32,
    ) != 0
    {
        return -22;
    }

    // SAFETY: caller's file; objects are looked up in its device.
    unsafe {
        let dev = (*file).dev;
        let crtc = drm_crtc_find(dev, req.crtc_id) as *mut Crtc;
        if crtc.is_null() || (*crtc).enabled == 0 {
            return -22;
        }

        let fb = drm_fb_find(dev, req.fb_id) as *mut Framebuffer;
        if fb.is_null() {
            return -22;
        }

        /* Install the new scanout before calling the driver, so a driver that
         * cannot flip asynchronously can simply return 0 and be correct. */
        if (*crtc).fb != fb as *mut c_void {
            if !(*crtc).fb.is_null() {
                drm_gem_unref((*((*crtc).fb as *mut Framebuffer)).obj);
            }
            (*crtc).fb = fb as *mut c_void;
            drm_gem_ref((*fb).obj);
        }

        let rc = drm_driver_page_flip(
            dev,
            crtc,
            fb,
            req.flags,
            req.user_data as u32 as usize as *mut c_void,
        );
        if rc != 0 {
            return rc;
        }

        drm_crtc_vblank_bump(crtc);

        if req.flags & DRM_MODE_PAGE_FLIP_EVENT != 0 {
            drm_file_queue_event(
                dev,
                crtc,
                DRM_EVENT_FLIP_COMPLETE,
                req.user_data,
                (*crtc).vblank_count,
            );
        }
    }
    0
}

/* ── cursor ─────────────────────────────────────────────────────────────── */

/// `struct drm_mode_cursor` — 28 bytes.
#[repr(C)]
pub struct ModeCursor {
    pub flags: u32,
    pub crtc_id: u32,
    pub x: i32,
    pub y: i32,
    pub width: u32,
    pub height: u32,
    pub handle: u32,
}

/// `struct drm_mode_cursor2` — 36 bytes: the same plus the hot spot.
#[repr(C)]
pub struct ModeCursor2 {
    pub flags: u32,
    pub crtc_id: u32,
    pub x: i32,
    pub y: i32,
    pub width: u32,
    pub height: u32,
    pub handle: u32,
    pub hot_x: i32,
    pub hot_y: i32,
}

const _: () = assert!(core::mem::size_of::<ModeCursor>() == 28);
const _: () = assert!(core::mem::offset_of!(ModeCursor, handle) == 24);
const _: () = assert!(core::mem::size_of::<ModeCursor2>() == 36);
const _: () = assert!(core::mem::offset_of!(ModeCursor2, hot_x) == 28);

const DRM_IOCTL_MODE_CURSOR: u32 = drm_iowr(0xA3, 28);
const DRM_IOCTL_MODE_CURSOR2: u32 = drm_iowr(0xBB, 36);
const DRM_MODE_CURSOR_BO: u32 = 0x01;
const DRM_MODE_CURSOR_MOVE: u32 = 0x02;
const DRM_MODE_CURSOR_FLAGS: u32 = 0x03;

/// Shared body of MODE_CURSOR / MODE_CURSOR2.  A `BO` request carries a new
/// image (a GEM handle, 0 to hide) and a `MOVE` request a new position; a
/// single ioctl may ask for either or both.
unsafe fn cursor_apply(
    file: *mut DrmFile,
    dev: *mut DrmDevice,
    flags: u32,
    crtc_id: u32,
    x: i32,
    y: i32,
    w: u32,
    h: u32,
    handle: u32,
    hot_x: i32,
    hot_y: i32,
) -> c_int {
    if dev.is_null() || flags == 0 || flags & !DRM_MODE_CURSOR_FLAGS != 0 {
        return -22;
    }
    let crtc = drm_crtc_find(dev, crtc_id) as *mut Crtc;
    if crtc.is_null() {
        return -22;
    }

    if flags & DRM_MODE_CURSOR_BO != 0 {
        /* handle 0 means "hide the cursor". */
        let obj = if handle == 0 {
            core::ptr::null_mut()
        } else {
            drm_gem_handle_lookup(file, handle)
        };
        if handle != 0 && obj.is_null() {
            return -22;
        }
        let ops = (*dev).ops;
        if !ops.is_null() {
            if let Some(f) = (*ops).cursor_set {
                let rc = f(crtc as *mut c_void, obj, w, h, hot_x, hot_y);
                if rc != 0 {
                    return rc;
                }
            }
        }
        (*crtc).cursor.handle = handle;
        (*crtc).cursor.obj = obj;
        (*crtc).cursor.w = w;
        (*crtc).cursor.h = h;
        (*crtc).cursor.hot_x = hot_x;
        (*crtc).cursor.hot_y = hot_y;
    }

    if flags & DRM_MODE_CURSOR_MOVE != 0 {
        let ops = (*dev).ops;
        if !ops.is_null() {
            if let Some(f) = (*ops).cursor_move {
                let rc = f(crtc as *mut c_void, x, y);
                if rc != 0 {
                    return rc;
                }
            }
        }
        (*crtc).cursor.x = x;
        (*crtc).cursor.y = y;
    }
    0
}

#[no_mangle]
pub extern "C" fn mode_cursor(file: *mut DrmFile, arg: *mut c_void) -> c_int {
    if file.is_null() {
        return -22;
    }
    let mut req = ModeCursor {
        flags: 0,
        crtc_id: 0,
        x: 0,
        y: 0,
        width: 0,
        height: 0,
        handle: 0,
    };
    if drm_copy_in(
        &mut req as *mut _ as *mut c_void,
        DRM_IOCTL_MODE_CURSOR,
        arg,
        core::mem::size_of::<ModeCursor>() as u32,
    ) != 0
    {
        return -22;
    }
    // SAFETY: caller's file.
    unsafe {
        cursor_apply(
            file,
            (*file).dev,
            req.flags,
            req.crtc_id,
            req.x,
            req.y,
            req.width,
            req.height,
            req.handle,
            0,
            0,
        )
    }
}

#[no_mangle]
pub extern "C" fn mode_cursor2(file: *mut DrmFile, arg: *mut c_void) -> c_int {
    if file.is_null() {
        return -22;
    }
    let mut req = ModeCursor2 {
        flags: 0,
        crtc_id: 0,
        x: 0,
        y: 0,
        width: 0,
        height: 0,
        handle: 0,
        hot_x: 0,
        hot_y: 0,
    };
    if drm_copy_in(
        &mut req as *mut _ as *mut c_void,
        DRM_IOCTL_MODE_CURSOR2,
        arg,
        core::mem::size_of::<ModeCursor2>() as u32,
    ) != 0
    {
        return -22;
    }
    // SAFETY: caller's file.
    unsafe {
        cursor_apply(
            file,
            (*file).dev,
            req.flags,
            req.crtc_id,
            req.x,
            req.y,
            req.width,
            req.height,
            req.handle,
            req.hot_x,
            req.hot_y,
        )
    }
}
