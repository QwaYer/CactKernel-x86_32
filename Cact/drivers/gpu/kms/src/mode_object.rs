//! The KMS object model: creating and looking up CRTCs, encoders, connectors
//! and planes.
//!
//! CRTCs and encoders are plain `#[repr(C)]` structs the driver reads.  A
//! connector's mode list and EDID, and a plane's fourcc list, are **not**
//! stored inline: they are `Vec`s owned by this side, with only the counts kept
//! in the driver-visible prefix.  That is what removes the old per-connector
//! mode cap and per-plane format cap.

use alloc::boxed::Box;
use alloc::vec::Vec;
use core::ffi::{c_int, c_void};

use crate::device::drm_driver_set_config;
use crate::ffi::drm_copy_in;
use crate::gem::{drm_gem_ref, drm_gem_unref};
use crate::kms::framebuffer::{drm_fb_find, Framebuffer};
use crate::mode::{DisplayMode, MODE_NAME_LEN};
use crate::structs::{pool_set, DrmDevice, DrmFile, GemObject, ModeSet};

extern "C" {

}

/* ── CRTC ────────────────────────────────────────────────────────────────── */

/// Cursor state the core keeps for a CRTC.  Core-only: a driver hears about
/// the cursor through its `cursor_set`/`cursor_move` ops, not through this.
pub struct CursorState {
    pub handle: u32,
    pub obj: *mut GemObject,
    pub x: i32,
    pub y: i32,
    pub hot_x: i32,
    pub hot_y: i32,
    pub w: u32,
    pub h: u32,
}

/// `struct drm_crtc` (drm_drv.h).
///
/// The driver-visible prefix is the first fields, ending at `connector`;
/// `cursor` is this side's own state, which is why the plane/cursor support
/// costs a driver nothing in the struct.
#[repr(C)]
pub struct Crtc {
    pub dev: *mut DrmDevice,
    pub id: u32,
    pub index: i32,
    pub name: [u8; MODE_NAME_LEN],
    /// `struct drm_framebuffer *`
    pub fb: *mut c_void,
    pub mode: DisplayMode,
    pub enabled: i32,
    pub x: i32,
    pub y: i32,
    pub vblank_count: u32,
    pub vblank_enabled: i32,
    /// `struct drm_connector *`
    pub connector: *mut c_void,

    pub cursor: CursorState,

    /// When the vblank counter was last brought up to date (microseconds).
    /// Core-only: the device has no vblank interrupt, so the core models
    /// vblanks from the clock and the mode's refresh rate.
    pub vblank_last_usec: u32,
}

const _: () = assert!(core::mem::offset_of!(Crtc, name) == 12);
const _: () = assert!(core::mem::offset_of!(Crtc, fb) == 44);
const _: () = assert!(core::mem::offset_of!(Crtc, mode) == 48);
const _: () = assert!(core::mem::offset_of!(Crtc, enabled) == 112);
const _: () = assert!(core::mem::offset_of!(Crtc, vblank_count) == 124);
const _: () = assert!(core::mem::offset_of!(Crtc, connector) == 132);

/* ── encoder ─────────────────────────────────────────────────────────────── */

/// `struct drm_encoder` (drm_drv.h) — 28 bytes.
#[repr(C)]
pub struct Encoder {
    pub dev: *mut DrmDevice,
    pub id: u32,
    pub index: i32,
    pub encoder_type: u32,
    pub possible_crtcs: u32,
    pub possible_clones: u32,
    pub crtc: *mut c_void,
}

const _: () = assert!(core::mem::size_of::<Encoder>() == 28);
const _: () = assert!(core::mem::offset_of!(Encoder, encoder_type) == 12);
const _: () = assert!(core::mem::offset_of!(Encoder, possible_crtcs) == 16);
const _: () = assert!(core::mem::offset_of!(Encoder, crtc) == 24);

/// Copy a C string into a fixed-size name field, always NUL-terminated
/// (`strlcpy(dst, src, N)`).
fn copy_name(dst: &mut [u8; MODE_NAME_LEN], src: *const u8) {
    let mut i = 0;
    while i < MODE_NAME_LEN - 1 {
        // SAFETY: the caller passes a NUL-terminated string (or NULL, checked by the
        // caller) and `i < MODE_NAME_LEN - 1`, so this byte pointer is in bounds.
        let p = unsafe { src.add(i) };
        // SAFETY: `p` points at one byte of that string.
        let b = unsafe { *p };
        dst[i] = b;
        if b == 0 {
            return;
        }
        i += 1;
    }
    dst[MODE_NAME_LEN - 1] = 0;
}

/// `"<prefix><n>"` (e.g. `crtc-1`) into a fixed-size name field.
fn write_indexed_name(dst: &mut [u8; MODE_NAME_LEN], prefix: &[u8], n: i32) {
    let mut len = 0;
    for &b in prefix {
        if len >= MODE_NAME_LEN - 1 {
            dst[MODE_NAME_LEN - 1] = 0;
            return;
        }
        dst[len] = b;
        len += 1;
    }

    let mut digits = [0u8; 10];
    let mut used = 0;
    let mut v = n.unsigned_abs();
    loop {
        digits[used] = b'0' + (v % 10) as u8;
        used += 1;
        v /= 10;
        if v == 0 {
            break;
        }
    }
    while used > 0 {
        used -= 1;
        if len >= MODE_NAME_LEN - 1 {
            break;
        }
        dst[len] = digits[used];
        len += 1;
    }
    dst[len] = 0;
}

/// `drm_mode_crtc_init` — allocate and register CRTC number `index`.
#[no_mangle]
pub extern "C" fn drm_mode_crtc_init(dev: *mut DrmDevice, index: i32, name: *const u8) -> c_int {
    if dev.is_null() || index < 0 {
        return -22;
    }
    let slot = index as usize;
    // SAFETY: `dev` is the caller's live device; this borrow of its CRTC pool is
    // consumed by the occupancy check.
    if dev_index_used(unsafe { (*dev).crtcs.as_slice() }, slot) {
        return -22;
    }
    let mem = cact_mm::kmalloc(core::mem::size_of::<Crtc>() as u32) as *mut Crtc;
    if mem.is_null() {
        return -12;
    }
    // SAFETY: `mem` is the fresh `kmalloc` block just checked non-null, so zeroing
    // one `Crtc` covers exactly that allocation.
    unsafe { core::ptr::write_bytes(mem, 0, 1) };
    {
        // SAFETY: `mem` is the fresh, exclusively-owned CRTC; this borrow is consumed
        // by the field stores below.
        let crtc = unsafe { &mut *mem };
        crtc.dev = dev;
        crtc.index = index;
        if name.is_null() {
            write_indexed_name(&mut crtc.name, b"crtc-", index + 1);
        } else {
            copy_name(&mut crtc.name, name);
        }

        // SAFETY: `dev` is the live device; claiming the slot in its CRTC pool.
        crtc.id = pool_set(unsafe { &mut (*dev).crtcs }, slot, mem as *mut c_void);

        /* The properties an atomic commit drives a CRTC through. */
        let active = crate::kms::property::drm_prop_crtc_active(crtc.dev);
        if active != 0 {
            let _ = crate::kms::property::drm_prop_attach(
                crtc.dev,
                active,
                DRM_MODE_OBJECT_CRTC,
                crtc.id,
                0,
            );
        }
        let mode_id = crate::kms::property::drm_prop_crtc_mode_id(crtc.dev);
        if mode_id != 0 {
            let _ = crate::kms::property::drm_prop_attach(
                crtc.dev,
                mode_id,
                DRM_MODE_OBJECT_CRTC,
                crtc.id,
                0,
            );
        }
    }
    0
}

/// `drm_mode_encoder_init` — allocate and register encoder number `index`.
#[no_mangle]
pub extern "C" fn drm_mode_encoder_init(
    dev: *mut DrmDevice,
    index: i32,
    etype: u32,
    possible_crtcs: u32,
    possible_clones: u32,
) -> c_int {
    if dev.is_null() || index < 0 {
        return -22;
    }
    let slot = index as usize;
    // SAFETY: `dev` is the caller's live device; this borrow of its encoder pool is
    // consumed by the occupancy check.
    if dev_index_used(unsafe { (*dev).encoders.as_slice() }, slot) {
        return -22;
    }
    let mem = cact_mm::kmalloc(core::mem::size_of::<Encoder>() as u32) as *mut Encoder;
    if mem.is_null() {
        return -12;
    }
    // SAFETY: `mem` is the fresh `kmalloc` block just checked non-null, so zeroing
    // one `Encoder` covers exactly that allocation.
    unsafe { core::ptr::write_bytes(mem, 0, 1) };
    {
        // SAFETY: `mem` is the fresh, exclusively-owned encoder; this borrow is
        // consumed by the field stores below.
        let enc = unsafe { &mut *mem };
        enc.dev = dev;
        enc.index = index;
        enc.encoder_type = etype;
        enc.possible_crtcs = possible_crtcs;
        enc.possible_clones = possible_clones;

        // SAFETY: `dev` is the live device; claiming the slot in its encoder pool.
        enc.id = pool_set(unsafe { &mut (*dev).encoders }, slot, mem as *mut c_void);
    }
    0
}

/// True when a pool slot is already claimed.
fn dev_index_used(pool: &[*mut c_void], index: usize) -> bool {
    matches!(pool.get(index), Some(&p) if !p.is_null())
}

/// `drm_crtc_find` — look a CRTC up by the id userspace uses.
#[no_mangle]
pub extern "C" fn drm_crtc_find(dev: *mut DrmDevice, id: u32) -> *mut c_void {
    if dev.is_null() {
        return core::ptr::null_mut();
    }
    // SAFETY: read-only walk of the caller's live device.
    unsafe { (*dev).find_crtc(id) }
}

/// `drm_encoder_find` — look an encoder up by id.
#[no_mangle]
pub extern "C" fn drm_encoder_find(dev: *mut DrmDevice, id: u32) -> *mut c_void {
    if dev.is_null() {
        return core::ptr::null_mut();
    }
    // SAFETY: read-only walk of the caller's live device.
    unsafe { (*dev).find_encoder(id) }
}

/* ── connector ───────────────────────────────────────────────────────────── */

/// `struct drm_connector` (drm_drv.h).
///
/// The first fields are the driver-visible prefix (`drm_drv.h` declares exactly
/// those, ending at `dpms`); `modes`/`edid` are this side's own storage, which
/// is why the lists are unbounded.
#[repr(C)]
pub struct Connector {
    pub dev: *mut DrmDevice,
    pub id: u32,
    pub index: i32,
    pub connector_type: u32,
    pub connector_type_id: u32,
    pub status: u32,
    pub mm_width: u32,
    pub mm_height: u32,
    pub count_modes: i32,
    pub edid_len: u32,
    pub encoder: *mut c_void,
    pub dpms: u32,

    pub modes: Vec<DisplayMode>,
    pub edid: Vec<u8>,
}

const _: () = assert!(core::mem::offset_of!(Connector, status) == 20);
const _: () = assert!(core::mem::offset_of!(Connector, mm_width) == 24);
const _: () = assert!(core::mem::offset_of!(Connector, count_modes) == 32);
const _: () = assert!(core::mem::offset_of!(Connector, edid_len) == 36);
const _: () = assert!(core::mem::offset_of!(Connector, encoder) == 40);
const _: () = assert!(core::mem::offset_of!(Connector, dpms) == 44);

/* ── plane ───────────────────────────────────────────────────────────────── */

/// `struct drm_plane` (drm_drv.h): the driver-visible prefix ends at
/// `format_type`, with the fourcc list kept behind it.
#[repr(C)]
pub struct Plane {
    pub dev: *mut DrmDevice,
    pub id: u32,
    pub index: i32,
    pub plane_type: u32,
    pub possible_crtcs: u32,
    pub format_count: i32,
    pub format_type: u32,

    pub formats: Vec<u32>,

    /* Core-only attach state: what GETPLANE reports and what SETPLANE records. */
    pub crtc_id: u32,
    pub fb_id: u32,
    pub crtc_x: i32,
    pub crtc_y: i32,
    pub crtc_w: u32,
    pub crtc_h: u32,
    pub src_x: u32,
    pub src_y: u32,
    pub src_w: u32,
    pub src_h: u32,
}

const _: () = assert!(core::mem::offset_of!(Plane, plane_type) == 12);
const _: () = assert!(core::mem::offset_of!(Plane, possible_crtcs) == 16);
const _: () = assert!(core::mem::offset_of!(Plane, format_count) == 20);
const _: () = assert!(core::mem::offset_of!(Plane, format_type) == 24);

/* uapi values the object model needs (drm_drv.h / uapi/drm_mode.h). */
const DRM_MODE_UNKNOWNCONNECTION: u32 = 3;
const DRM_MODE_DPMS_ON: u32 = 0;
const DRM_MODE_DPMS_STANDBY: u64 = 1;
const DRM_MODE_DPMS_SUSPEND: u64 = 2;
const DRM_MODE_DPMS_OFF: u64 = 3;
const DRM_MODE_PROP_ENUM: u32 = 1 << 3;
const DRM_MODE_OBJECT_CRTC: u32 = 0xcccc_cccc;
const DRM_MODE_OBJECT_CONNECTOR: u32 = 0xc0c0_c0c0;
const DRM_MODE_OBJECT_PLANE: u32 = 0xeeee_eeee;

/// The mandatory connector property: DPMS.
fn add_std_props(conn: *mut Connector) {
    static DPMS_ON: &[u8] = b"On\0";
    static DPMS_STANDBY: &[u8] = b"Standby\0";
    static DPMS_SUSPEND: &[u8] = b"Suspend\0";
    static DPMS_OFF: &[u8] = b"Off\0";

    let values: [u64; 4] = [
        DRM_MODE_DPMS_ON as u64,
        DRM_MODE_DPMS_STANDBY,
        DRM_MODE_DPMS_SUSPEND,
        DRM_MODE_DPMS_OFF,
    ];
    let names: [*const u8; 4] = [
        DPMS_ON.as_ptr(),
        DPMS_STANDBY.as_ptr(),
        DPMS_SUSPEND.as_ptr(),
        DPMS_OFF.as_ptr(),
    ];

    // SAFETY: `conn` is the caller's freshly created connector; this borrow is
    // exclusive for the property setup below (the property helpers only take its
    // device and ids).
    let conn = unsafe { &mut *conn };
    let id = crate::kms::property::drm_prop_create(
        conn.dev,
        c"DPMS".as_ptr() as *const u8,
        DRM_MODE_PROP_ENUM,
        DRM_MODE_PROP_ENUM,
        4,
        values.as_ptr(),
        names.as_ptr(),
        8,
    );
    if id != 0 {
        let _ = crate::kms::property::drm_prop_attach(
            conn.dev,
            id,
            DRM_MODE_OBJECT_CONNECTOR,
            conn.id,
            conn.dpms as u64,
        );
    }

    /* The EDID blob property exists on every connector from the start, with a
     * zero blob id until drm_connector_set_edid() publishes one. */
    let edid = crate::kms::property::drm_prop_edid_id(conn.dev);
    if edid != 0 {
        let _ = crate::kms::property::drm_prop_attach(
            conn.dev,
            edid,
            DRM_MODE_OBJECT_CONNECTOR,
            conn.id,
            0,
        );
    }

    /* CRTC_ID: which CRTC the connector drives, for the atomic API. */
    let crtc_id = crate::kms::property::drm_prop_connector_crtc_id(conn.dev);
    if crtc_id != 0 {
        let _ = crate::kms::property::drm_prop_attach(
            conn.dev,
            crtc_id,
            DRM_MODE_OBJECT_CONNECTOR,
            conn.id,
            0,
        );
    }
}

/// `drm_mode_connector_init` — allocate and register connector `index`.
#[no_mangle]
pub extern "C" fn drm_mode_connector_init(
    dev: *mut DrmDevice,
    index: i32,
    ctype: u32,
    type_id: u32,
) -> c_int {
    if dev.is_null() || index < 0 {
        return -22;
    }
    let slot = index as usize;
    // SAFETY: `dev` is the caller's live device; this borrow of its connector pool
    // is consumed by the occupancy check.
    if dev_index_used(unsafe { (*dev).connectors.as_slice() }, slot) {
        return -22;
    }

    {

        let conn = Box::new(Connector {
            dev,
            id: 0,
            index,
            connector_type: ctype,
            connector_type_id: if type_id != 0 { type_id } else { 1 },
            status: DRM_MODE_UNKNOWNCONNECTION,
            mm_width: 0,
            mm_height: 0,
            count_modes: 0,
            edid_len: 0,
            encoder: core::ptr::null_mut(),
            dpms: DRM_MODE_DPMS_ON,
            modes: Vec::new(),
            edid: Vec::new(),
        });
        let mem = Box::into_raw(conn);
        // SAFETY: `dev` is the live device; claiming the slot in its connector pool.
        let id = pool_set(unsafe { &mut (*dev).connectors }, slot, mem as *mut c_void);
        // SAFETY: `mem` is the fresh connector; storing its id.
        unsafe { (*mem).id = id };
        add_std_props(mem);
    }
    0
}

/// `drm_mode_plane_init` — allocate and register plane `index`.
#[no_mangle]
pub extern "C" fn drm_mode_plane_init(
    dev: *mut DrmDevice,
    index: i32,
    ptype: u32,
    possible_crtcs: u32,
    formats: *const u32,
    format_count: c_int,
    format_type: u32,
) -> c_int {
    if dev.is_null() || index < 0 {
        return -22;
    }
    let slot = index as usize;
    // SAFETY: `dev` is the caller's live device; this borrow of its plane pool is
    // consumed by the occupancy check.
    if dev_index_used(unsafe { (*dev).planes.as_slice() }, slot) {
        return -22;
    }

    {
        let mut list: Vec<u32> = Vec::new();
        if !formats.is_null() && format_count > 0 {
            list.reserve(format_count as usize);
            for i in 0..format_count as usize {
                // SAFETY: the caller passes `format_count` readable `u32`s and
                // `i < format_count`, so this element pointer is in bounds.
                let f = unsafe { formats.add(i) };
                // SAFETY: `f` points at one `u32` pixel format.
                list.push(unsafe { *f });
            }
        }

        let plane = Box::new(Plane {
            dev,
            id: 0,
            index,
            plane_type: ptype,
            possible_crtcs,
            format_count: list.len() as i32,
            format_type,
            formats: list,
            crtc_id: 0,
            fb_id: 0,
            crtc_x: 0,
            crtc_y: 0,
            crtc_w: 0,
            crtc_h: 0,
            src_x: 0,
            src_y: 0,
            src_w: 0,
            src_h: 0,
        });
        let mem = Box::into_raw(plane);
        // SAFETY: `dev` is the live device; claiming the slot in its plane pool.
        let id = pool_set(unsafe { &mut (*dev).planes }, slot, mem as *mut c_void);
        // SAFETY: `mem` is the fresh plane; storing its id.
        unsafe { (*mem).id = id };

        /* Every plane carries the universal-planes `type` property, which is
         * how userspace tells a primary plane from a cursor plane (the uapi
         * GETPLANE carries no type field). */
        // SAFETY: `mem` is the fresh, exclusively-owned plane; this borrow is
        // consumed by the property setup below.
        let plane_ref = unsafe { &mut *mem };
        let type_prop = crate::kms::property::drm_prop_plane_type_id(plane_ref.dev);
        if type_prop != 0 {
            let _ = crate::kms::property::drm_prop_attach(
                plane_ref.dev,
                type_prop,
                DRM_MODE_OBJECT_PLANE,
                plane_ref.id,
                ptype as u64,
            );
        }

        /* The properties an atomic commit drives a plane through: which CRTC and
         * framebuffer it is attached to, and its rectangles. */
        for prop in [
            crate::kms::property::drm_prop_plane_crtc_id(plane_ref.dev),
            crate::kms::property::drm_prop_plane_fb_id(plane_ref.dev),
        ] {
            if prop != 0 {
                let _ = crate::kms::property::drm_prop_attach(
                    plane_ref.dev,
                    prop,
                    DRM_MODE_OBJECT_PLANE,
                    plane_ref.id,
                    0,
                );
            }
        }
        for prop in crate::kms::property::drm_prop_plane_rects(plane_ref.dev) {
            if prop != 0 {
                let _ = crate::kms::property::drm_prop_attach(
                    plane_ref.dev,
                    prop,
                    DRM_MODE_OBJECT_PLANE,
                    plane_ref.id,
                    0,
                );
            }
        }
    }
    0
}

/// `drm_connector_find` — look a connector up by id.
#[no_mangle]
pub extern "C" fn drm_connector_find(dev: *mut DrmDevice, id: u32) -> *mut c_void {
    if dev.is_null() {
        return core::ptr::null_mut();
    }
    // SAFETY: read-only walk of the caller's live device.
    unsafe { (*dev).find_connector(id) }
}

/// `drm_plane_find` — look a plane up by id.
#[no_mangle]
pub extern "C" fn drm_plane_find(dev: *mut DrmDevice, id: u32) -> *mut c_void {
    if dev.is_null() {
        return core::ptr::null_mut();
    }
    // SAFETY: read-only walk of the caller's live device.
    unsafe { (*dev).find_plane(id) }
}

/// `drm_connector_add_mode` — append a mode, ignoring duplicates.
///
/// Drivers usually recompute their mode list on every probe, so the same mode
/// arriving twice must not grow the list without bound.
#[no_mangle]
pub extern "C" fn drm_connector_add_mode(conn: *mut Connector, mode: *const DisplayMode) {
    if conn.is_null() || mode.is_null() {
        return;
    }
    // SAFETY: `conn` is the caller's live connector; this borrow is consumed by the
    // copy below.
    let conn = unsafe { &mut *conn };
    // SAFETY: `mode` is the caller's live display mode.
    let mode = unsafe { &*mode };
    {
        for m in &conn.modes {
            if m.hdisplay == mode.hdisplay && m.vdisplay == mode.vdisplay && m.clock == mode.clock {
                return;
            }
        }
        conn.modes.push(*mode);
        conn.count_modes = conn.modes.len() as i32;
    }
}

/// `drm_connector_set_edid` — copy an EDID block into the connector.
#[no_mangle]
pub extern "C" fn drm_connector_set_edid(conn: *mut Connector, edid: *const c_void, len: u32) {
    if conn.is_null() || edid.is_null() {
        return;
    }
    // SAFETY: `edid` points at `len` readable bytes (the caller's contract), so this
    // shared slice spans them.
    let src = unsafe { core::slice::from_raw_parts(edid as *const u8, len as usize) };
    // SAFETY: `conn` is the caller's live connector; this borrow is consumed by the
    // copy below.
    let conn = unsafe { &mut *conn };
    conn.edid.clear();
    conn.edid.extend_from_slice(src);
    conn.edid_len = len;

    /* Publish it as the connector's `EDID` blob property — that is how a client
     * reads the EDID, since drm_mode_get_connector carries no EDID pointer of its
     * own. */
    let dev = conn.dev;
    if !dev.is_null() {
        let prop = crate::kms::property::drm_prop_edid_id(dev);
        if prop != 0 {
            // SAFETY: `dev` is the live device; creating the blob from `src`.
            let blob_id = unsafe { (*dev).blob_create(src) };
            let _ = crate::kms::property::drm_prop_attach(
                dev,
                prop,
                DRM_MODE_OBJECT_CONNECTOR,
                conn.id,
                blob_id as u64,
            );
        }
    }
}

/// `drm_connector_attach_encoder`.
#[no_mangle]
pub extern "C" fn drm_connector_attach_encoder(conn: *mut Connector, enc: *mut c_void) {
    if conn.is_null() || enc.is_null() {
        return;
    }
    // SAFETY: caller's connector.
    unsafe {
        (*conn).encoder = enc;
    }
}

/// `drm_encoder_attach_crtc`.
#[no_mangle]
pub extern "C" fn drm_encoder_attach_crtc(enc: *mut Encoder, crtc: *mut c_void) {
    if enc.is_null() {
        return;
    }
    // SAFETY: caller's encoder.
    unsafe {
        (*enc).crtc = crtc;
    }
}

/* ── SETPLANE ───────────────────────────────────────────────────────────── */

/// `struct drm_mode_set_plane` — 48 bytes.  Note the source-rectangle order:
/// the uapi lists `src_h` before `src_w`.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct ModeSetPlane {
    pub plane_id: u32,
    pub crtc_id: u32,
    pub fb_id: u32,
    pub flags: u32,
    pub crtc_x: i32,
    pub crtc_y: i32,
    pub crtc_w: u32,
    pub crtc_h: u32,
    pub src_x: u32,
    pub src_y: u32,
    pub src_h: u32,
    pub src_w: u32,
}

const _: () = assert!(core::mem::size_of::<ModeSetPlane>() == 48);
const _: () = assert!(core::mem::offset_of!(ModeSetPlane, crtc_w) == 24);
const _: () = assert!(core::mem::offset_of!(ModeSetPlane, src_x) == 32);
const _: () = assert!(core::mem::offset_of!(ModeSetPlane, src_h) == 40);
const _: () = assert!(core::mem::offset_of!(ModeSetPlane, src_w) == 44);

const DRM_IOCTL_MODE_SETPLANE: u32 = (3u32 << 30) | (0x64u32 << 8) | 0xB7 | (48 << 16);

/// `DRM_IOCTL_MODE_SETPLANE`.
///
/// On this device the primary plane *is* the CRTC's scanout, so attaching a
/// framebuffer to it goes through the same `set_config` the legacy SETCRTC
/// uses — there is no separate plane hardware to program.  The plane's own
/// state (which CRTC/framebuffer, and the rectangles) is what GETPLANE reports
/// and what an atomic commit reads back.
#[no_mangle]
pub extern "C" fn mode_set_plane(file: *mut DrmFile, arg: *mut c_void) -> c_int {
    if file.is_null() {
        return -22;
    }
    let mut req = ModeSetPlane {
        plane_id: 0,
        crtc_id: 0,
        fb_id: 0,
        flags: 0,
        crtc_x: 0,
        crtc_y: 0,
        crtc_w: 0,
        crtc_h: 0,
        src_x: 0,
        src_y: 0,
        src_h: 0,
        src_w: 0,
    };
    if drm_copy_in(
        &mut req as *mut _ as *mut c_void,
        DRM_IOCTL_MODE_SETPLANE,
        arg,
        core::mem::size_of::<ModeSetPlane>() as u32,
    ) != 0
    {
        return -22;
    }

    // SAFETY: `file` is the caller's live client (checked non-null above); this reads
    // its device pointer.
    let dev = unsafe { (*file).dev };
    let plane = drm_plane_find(dev, req.plane_id) as *mut Plane;
    if plane.is_null() {
        return -22;
    }

    /* crtc_id == 0 (or fb_id == 0) turns the plane off: disable whatever scanout it
     * was driving, then forget the attach. */
    if req.crtc_id == 0 || req.fb_id == 0 {
        // SAFETY: `plane` is the live plane; this reads its attached CRTC id.
        let plane_crtc_id = unsafe { (*plane).crtc_id };
        let crtc = drm_crtc_find(dev, plane_crtc_id) as *mut Crtc;
        if !crtc.is_null() {
            // SAFETY: `crtc` is the live CRTC; this reads its current mode.
            let crtc_mode = unsafe { (*crtc).mode };
            let mut set = ModeSet {
                fb: core::ptr::null_mut(),
                crtc: crtc as *mut c_void,
                mode: crtc_mode,
                x: 0,
                y: 0,
                connectors: core::ptr::null_mut(),
                num_connectors: 0,
            };
            let rc = drm_driver_set_config(dev, &mut set);
            if rc != 0 {
                return rc;
            }
            // SAFETY: `crtc` is the live CRTC; this reads its framebuffer.
            let old_fb = unsafe { (*crtc).fb };
            if !old_fb.is_null() {
                // SAFETY: `old_fb` is a live framebuffer; this reads the GEM object it
                // holds a reference to.
                let obj = unsafe { (*(old_fb as *mut Framebuffer)).obj };
                drm_gem_unref(obj);
            }
            // SAFETY: as above — clearing the CRTC's framebuffer.
            unsafe { (*crtc).fb = core::ptr::null_mut() };
            // SAFETY: as above — marking the CRTC disabled.
            unsafe { (*crtc).enabled = 0 };
        }
        // SAFETY: `clear_plane`'s contract: `plane` is a live plane owned by the
        // caller.
        unsafe { clear_plane(plane) };
        return 0;
    }

    let crtc = drm_crtc_find(dev, req.crtc_id) as *mut Crtc;
    if crtc.is_null() {
        return -22;
    }

    let fb = drm_fb_find(dev, req.fb_id) as *mut Framebuffer;
    if fb.is_null() {
        return -22;
    }
    if req.crtc_w == 0 || req.crtc_h == 0 || req.src_w == 0 || req.src_h == 0 {
        return -22;
    }
    /* The plane has to be able to scan the format out. */
    // SAFETY: `plane` is the live plane; this borrow of its format list is consumed
    // by the scan.
    let formats = unsafe { &(*plane).formats };
    // SAFETY: `fb` is the live framebuffer; this reads its pixel format.
    let fb_format = unsafe { (*fb).format };
    if !formats.contains(&fb_format) {
        return -22;
    }

    // SAFETY: `crtc` is the live CRTC; this reads its current mode.
    let crtc_mode = unsafe { (*crtc).mode };
    let mut set = ModeSet {
        fb: fb as *mut c_void,
        crtc: crtc as *mut c_void,
        mode: crtc_mode,
        x: req.crtc_x,
        y: req.crtc_y,
        connectors: core::ptr::null_mut(),
        num_connectors: 0,
    };
    let rc = drm_driver_set_config(dev, &mut set);
    if rc != 0 {
        return rc;
    }

    /* Publish only after the driver accepted the mode.  The CRTC holds the
     * framebuffer reference; the plane just names it. */
    // SAFETY: `crtc` is the live CRTC; this reads its framebuffer.
    let old_fb = unsafe { (*crtc).fb };
    if old_fb != fb as *mut c_void {
        if !old_fb.is_null() {
            // SAFETY: `old_fb` is a live framebuffer; this reads the GEM object it
            // holds a reference to.
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
    // SAFETY: as above — marking the CRTC enabled.
    unsafe { (*crtc).enabled = 1 };

    // SAFETY: `plane` is the live plane; this borrow is consumed by the write-back of
    // its attach state.
    let plane = unsafe { &mut *plane };
    plane.crtc_id = req.crtc_id;
    plane.fb_id = req.fb_id;
    plane.crtc_x = req.crtc_x;
    plane.crtc_y = req.crtc_y;
    plane.crtc_w = req.crtc_w;
    plane.crtc_h = req.crtc_h;
    plane.src_x = req.src_x;
    plane.src_y = req.src_y;
    plane.src_w = req.src_w;
    plane.src_h = req.src_h;
    0
}

/// Forget a plane's attach state.
unsafe fn clear_plane(plane: *mut Plane) {
    // SAFETY: `plane` is a live plane owned by the caller (mode_set_plane checks it
    // against the device's plane pool before calling); this borrow is exclusive and
    // consumed by the field clears below.
    let plane = unsafe { &mut *plane };
    plane.crtc_id = 0;
    plane.fb_id = 0;
    plane.crtc_x = 0;
    plane.crtc_y = 0;
    plane.crtc_w = 0;
    plane.crtc_h = 0;
    plane.src_x = 0;
    plane.src_y = 0;
    plane.src_w = 0;
    plane.src_h = 0;
}
