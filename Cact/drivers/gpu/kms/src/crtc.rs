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
use crate::gem::{drm_gem_handle_lookup, drm_gem_ref, drm_gem_unref};
use crate::mode::{drm_mode_to_modeinfo, drm_modeinfo_to_mode, DisplayMode, ModeInfo, MODE_NAME_LEN};
use crate::structs::{DrmDevice, DrmFile, ModeSet};

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

    // SAFETY: `c` is the live CRTC looked up above; this borrow is consumed by the
    // field reads below.
    let c = unsafe { &*c };
    let mut mi = empty_mode();
    drm_mode_to_modeinfo(&c.mode, &mut mi);

    req.set_connectors_ptr = 0;
    req.count_connectors = 0;
    req.fb_id = if c.fb.is_null() {
        0
    } else {
        // SAFETY: `c.fb` is a live framebuffer (non-null, checked above); this reads
        // its id.
        unsafe { (*(c.fb as *mut Framebuffer)).id }
    };
    req.x = c.x as u32;
    req.y = c.y as u32;
    req.gamma_size = 0;
    req.mode_valid = if c.enabled != 0 { 1 } else { 0 };
    /* drm_mode_get_crtc carries the mode inline, not by pointer. */
    req.mode = mi;

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

    // SAFETY: `file` is the caller's live client (checked non-null above); this reads
    // its device pointer.
    let dev = unsafe { (*file).dev };
    let crtc = drm_crtc_find(dev, req.crtc_id) as *mut Crtc;
    if crtc.is_null() {
        return -22;
    }

    /* fb_id == 0 turns the CRTC off, which is how a compositor releases the display
     * (drmModeSetCrtc(fd, crtc, 0, 0, 0, NULL, 0)). */
    if req.fb_id == 0 {
        let mut set = empty_set();
        set.crtc = crtc as *mut c_void;
        // SAFETY: `crtc` is the live CRTC; this reads its current mode.
        set.mode = unsafe { (*crtc).mode };
        let rc = drm_driver_set_config(dev, &mut set);
        if rc != 0 {
            return rc;
        }
        // SAFETY: `crtc` is the live CRTC; this reads its framebuffer.
        let old_fb = unsafe { (*crtc).fb };
        if !old_fb.is_null() {
            // SAFETY: `old_fb` is a live framebuffer; this reads its GEM object.
            let obj = unsafe { (*(old_fb as *mut Framebuffer)).obj };
            drm_gem_unref(obj);
        }
        // SAFETY: as above — clearing the framebuffer.
        unsafe { (*crtc).fb = core::ptr::null_mut() };
        // SAFETY: as above — marking the CRTC disabled.
        unsafe { (*crtc).enabled = 0 };
        // SAFETY: as above — clearing its connector.
        unsafe { (*crtc).connector = core::ptr::null_mut() };
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

    /* The connector list is rebuilt here (it lives no longer than the call) and is
     * where the encoder/CRTC binding is established on the legacy path. */
    let mut conns: Vec<*mut c_void> = Vec::new();
    if req.count_connectors != 0 && req.set_connectors_ptr != 0 {
        let count = req.count_connectors as usize;
        let n = if count > MAX_SET_CONNECTORS {
            MAX_SET_CONNECTORS
        } else {
            count
        };
        let mut ids: Vec<u32> = alloc::vec![0u32; n];
        let user = req.set_connectors_ptr as u32 as usize as *const c_void;
        let bytes = (n * 4) as u32;
        // SAFETY: `user` is the client's pointer; this range-checks it.
        if unsafe { validate_user_ptr(user, bytes) } == 0 {
            return -22;
        }
        // SAFETY: the range check above passed, so `user` is a readable userspace
        // range and `ids` is a writable buffer of the same size.
        if unsafe { copy_from_user(ids.as_mut_ptr() as *mut c_void, user, bytes) } != 0 {
            return -22;
        }

        // SAFETY: `dev` is the live device; this borrow of its encoder pool is
        // consumed by the scan below.
        let encoders = unsafe { &(*dev).encoders };
        for &id in ids.iter() {
            let c = drm_connector_find(dev, id) as *mut Connector;
            if c.is_null() {
                return -22;
            }
            conns.push(c as *mut c_void);

            // SAFETY: `c` is the live connector (non-null, checked above); this reads
            // its encoder.
            let cur_enc = unsafe { (*c).encoder };
            if cur_enc.is_null() {
                /* Pick any encoder that can drive this CRTC. */
                // SAFETY: `crtc` is the live CRTC; this reads its index.
                let bit = 1u32 << unsafe { (*crtc).index };
                for &e in encoders.iter() {
                    let enc = e as *mut Encoder;
                    if enc.is_null() {
                        continue;
                    }
                    // SAFETY: `enc` is a live encoder (non-null, checked above); this
                    // reads its CRTC mask.
                    if unsafe { (*enc).possible_crtcs } & bit != 0 {
                        drm_connector_attach_encoder(c, e);
                        break;
                    }
                }
            }
            // SAFETY: `c` is the live connector; this re-reads its encoder.
            let cur_enc = unsafe { (*c).encoder };
            if !cur_enc.is_null() {
                drm_encoder_attach_crtc(cur_enc as *mut Encoder, crtc as *mut c_void);
            }
        }
    }
    if conns.is_empty() {
        return -22;
    }
    set.connectors = conns.as_mut_ptr();
    set.num_connectors = conns.len() as i32;

    /* The driver programs the hardware first: only when it accepts the mode do we
     * publish the new state to userspace. */
    let rc = drm_driver_set_config(dev, &mut set);
    if rc != 0 {
        return rc;
    }

    // SAFETY: `crtc` is the live CRTC; this reads its framebuffer.
    let old_fb = unsafe { (*crtc).fb };
    if !old_fb.is_null() && old_fb != fb as *mut c_void {
        // SAFETY: `old_fb` is a live framebuffer; this reads its GEM object.
        let obj = unsafe { (*(old_fb as *mut Framebuffer)).obj };
        drm_gem_unref(obj);
    }
    // SAFETY: `crtc` is the live CRTC; these publish the new state.
    unsafe { (*crtc).fb = fb as *mut c_void };
    // SAFETY: as above — the new mode.
    unsafe { (*crtc).mode = set.mode };
    // SAFETY: as above — the new position.
    unsafe { (*crtc).x = set.x };
    // SAFETY: as above — the new position.
    unsafe { (*crtc).y = set.y };
    // SAFETY: as above — marking the CRTC enabled.
    unsafe { (*crtc).enabled = 1 };
    // SAFETY: as above — the first connector drives it.
    unsafe { (*crtc).connector = conns[0] };

    // SAFETY: `fb` is the live framebuffer; this reads its GEM object to take a
    // reference.
    let obj = unsafe { (*fb).obj };
    drm_gem_ref(obj);
    drm_crtc_vblank_bump(crtc);
    // SAFETY: `crtc` is the live CRTC; this reads its vblank-enabled flag.
    let vblank_enabled = unsafe { (*crtc).vblank_enabled };
    if vblank_enabled == 0 && drm_driver_enable_vblank(dev, crtc) == 0 {
        // SAFETY: as above — recording that vblank is now enabled.
        unsafe { (*crtc).vblank_enabled = 1 };
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

    // SAFETY: `file` is the caller's live client (checked non-null above); this reads
    // its device pointer.
    let dev = unsafe { (*file).dev };
    let crtc = drm_crtc_find(dev, req.crtc_id) as *mut Crtc;
    // SAFETY: `crtc` is either null (checked first) or the live CRTC; this reads its
    // enabled flag.
    if crtc.is_null() || unsafe { (*crtc).enabled } == 0 {
        return -22;
    }

    let fb = drm_fb_find(dev, req.fb_id) as *mut Framebuffer;
    if fb.is_null() {
        return -22;
    }

    /* Install the new scanout before calling the driver, so a driver that cannot
     * flip asynchronously can simply return 0 and be correct. */
    // SAFETY: `crtc` is the live CRTC; this reads its framebuffer.
    let old_fb = unsafe { (*crtc).fb };
    if old_fb != fb as *mut c_void {
        if !old_fb.is_null() {
            // SAFETY: `old_fb` is a live framebuffer; this reads its GEM object.
            let obj = unsafe { (*(old_fb as *mut Framebuffer)).obj };
            drm_gem_unref(obj);
        }
        // SAFETY: as above — installing the new framebuffer.
        unsafe { (*crtc).fb = fb as *mut c_void };
        // SAFETY: `fb` is the live framebuffer; this reads its GEM object to take a
        // reference.
        let obj = unsafe { (*fb).obj };
        drm_gem_ref(obj);
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
        // SAFETY: `crtc` is the live CRTC; this reads its vblank sequence, as the
        // event should report.
        let seq = unsafe { (*crtc).vblank_count };
        drm_file_queue_event(dev, crtc, DRM_EVENT_FLIP_COMPLETE, req.user_data, seq);
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
// The argument list mirrors the union of the `drm_mode_cursor` / `drm_mode_cursor2`
// uapi structs this is the shared body of; folding it into a struct would only
// move the same fields one level down, so the count is accepted deliberately.
#[allow(clippy::too_many_arguments)]
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
            // `drm_gem_handle_lookup` only walks the caller's own handle table, so
            // it is an ordinary safe call.
            drm_gem_handle_lookup(file, handle)
        };
        if handle != 0 && obj.is_null() {
            return -22;
        }
        // SAFETY: `dev` is the caller's live device; this copies its ops pointer.
        let ops = unsafe { (*dev).ops };
        if !ops.is_null() {
            // SAFETY: `ops` is the driver's live ops table; this copies the
            // `cursor_set` fn pointer only.
            let f = unsafe { (*ops).cursor_set };
            if let Some(f) = f {
                let rc = f(crtc as *mut c_void, obj, w, h, hot_x, hot_y);
                if rc != 0 {
                    return rc;
                }
            }
        }
        // SAFETY: `crtc` is the live CRTC just resolved from the device's pool; these
        // store the new cursor image state.
        unsafe { (*crtc).cursor.handle = handle };
        // SAFETY: as above — the object.
        unsafe { (*crtc).cursor.obj = obj };
        // SAFETY: as above — the width.
        unsafe { (*crtc).cursor.w = w };
        // SAFETY: as above — the height.
        unsafe { (*crtc).cursor.h = h };
        // SAFETY: as above — the hot spot.
        unsafe { (*crtc).cursor.hot_x = hot_x };
        // SAFETY: as above — the hot spot.
        unsafe { (*crtc).cursor.hot_y = hot_y };
    }

    if flags & DRM_MODE_CURSOR_MOVE != 0 {
        // SAFETY: `dev` is the caller's live device; this copies its ops pointer.
        let ops = unsafe { (*dev).ops };
        if !ops.is_null() {
            // SAFETY: `ops` is the driver's live ops table; this copies the
            // `cursor_move` fn pointer only.
            let f = unsafe { (*ops).cursor_move };
            if let Some(f) = f {
                let rc = f(crtc as *mut c_void, x, y);
                if rc != 0 {
                    return rc;
                }
            }
        }
        // SAFETY: `crtc` is the live CRTC; these store the new cursor position.
        unsafe { (*crtc).cursor.x = x };
        // SAFETY: as above — the position.
        unsafe { (*crtc).cursor.y = y };
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
    // SAFETY: `file` is the caller's live client (checked non-null above); this reads
    // its device pointer.
    let dev = unsafe { (*file).dev };
    // SAFETY: `cursor_apply`'s contract: `file`/`dev` are the caller's live
    // client/device.
    unsafe {
        cursor_apply(
            file, dev, req.flags, req.crtc_id, req.x, req.y, req.width, req.height, req.handle, 0,
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
    // SAFETY: `file` is the caller's live client (checked non-null above); this reads
    // its device pointer.
    let dev = unsafe { (*file).dev };
    // SAFETY: `cursor_apply`'s contract: `file`/`dev` are the caller's live
    // client/device.
    unsafe {
        cursor_apply(
            file, dev, req.flags, req.crtc_id, req.x, req.y, req.width, req.height, req.handle,
            req.hot_x, req.hot_y,
        )
    }
}
