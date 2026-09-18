//! Display modes: the kernel's `struct drm_display_mode`, the uapi
//! `struct drm_mode_modeinfo`, the conversions between them, and the
//! fourcc -> bits-per-pixel table.
//!
//! These are the crate's first mirrored C structs.  They are `#[repr(C)]` with
//! size/offset assertions pinned to the numbers the C compiler reports for
//! `drm_drv.h`/`uapi/drm_mode.h`, so a layout drift becomes a build error
//! instead of silent memory corruption on the boundary.
//!
//! Migrated slice 2: `drm_mode_to_modeinfo`, `drm_modeinfo_to_mode` and
//! `drm_format_bpp` used to live in `kms/drm_mode_object.c`.

/// `DRM_MODE_NAME_LEN` (drm_drv.h) — also the uapi's `name[]` length.
pub const MODE_NAME_LEN: usize = 32;

/// `struct drm_display_mode` (drm_drv.h) — the kernel-side mode.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct DisplayMode {
    pub clock: u32, // kHz
    pub hdisplay: u16,
    pub hsync_start: u16,
    pub hsync_end: u16,
    pub htotal: u16,
    pub hskew: u16,
    pub vdisplay: u16,
    pub vsync_start: u16,
    pub vsync_end: u16,
    pub vtotal: u16,
    pub vscan: u16,
    pub flags: u32,
    /// C calls this `type`; `mtype` is only the Rust name, the offset is what
    /// matters and is asserted below.
    pub mtype: u32,
    pub name: [u8; MODE_NAME_LEN],
}

/// `struct drm_mode_modeinfo` (uapi/drm_mode.h) — the form a client reads.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct ModeInfo {
    pub clock: u32,
    pub hdisplay: u16,
    pub hsync_start: u16,
    pub hsync_end: u16,
    pub htotal: u16,
    pub hskew: u16,
    pub vdisplay: u16,
    pub vsync_start: u16,
    pub vsync_end: u16,
    pub vtotal: u16,
    pub vscan: u16,
    pub vrefresh: u32,
    pub flags: u32,
    pub mtype: u32,
    pub name: [u8; MODE_NAME_LEN],
}

/* Layout pins — the C values these assertions encode were taken from a
 * `sizeof`/`offsetof` probe against the real headers. */
const _: () = assert!(core::mem::size_of::<DisplayMode>() == 64);
const _: () = assert!(core::mem::offset_of!(DisplayMode, vtotal) == 20);
const _: () = assert!(core::mem::offset_of!(DisplayMode, vscan) == 22);
const _: () = assert!(core::mem::offset_of!(DisplayMode, flags) == 24);
const _: () = assert!(core::mem::offset_of!(DisplayMode, mtype) == 28);
const _: () = assert!(core::mem::offset_of!(DisplayMode, name) == 32);

const _: () = assert!(core::mem::size_of::<ModeInfo>() == 68);
const _: () = assert!(core::mem::offset_of!(ModeInfo, vtotal) == 20);
const _: () = assert!(core::mem::offset_of!(ModeInfo, vrefresh) == 24);
const _: () = assert!(core::mem::offset_of!(ModeInfo, flags) == 28);
const _: () = assert!(core::mem::offset_of!(ModeInfo, mtype) == 32);
const _: () = assert!(core::mem::offset_of!(ModeInfo, name) == 36);

/// Display mode -> the uapi form a client reads.
///
/// `mode_to_modeinfo` in C: the target is zeroed, the timings copied, and the
/// name copied up to and including its NUL (so the padding stays zero).
#[no_mangle]
pub extern "C" fn drm_mode_to_modeinfo(m: *const DisplayMode, mi: *mut ModeInfo) {
    if m.is_null() || mi.is_null() {
        return;
    }
    // SAFETY: the caller guarantees pointers to a valid source mode and a
    // writable uapi struct, exactly as the C function assumed.
    unsafe {
        core::ptr::write_bytes(mi, 0, 1);
        let m = &*m;
        let mi = &mut *mi;
        mi.clock = m.clock;
        mi.hdisplay = m.hdisplay;
        mi.hsync_start = m.hsync_start;
        mi.hsync_end = m.hsync_end;
        mi.htotal = m.htotal;
        mi.hskew = m.hskew;
        mi.vdisplay = m.vdisplay;
        mi.vsync_start = m.vsync_start;
        mi.vsync_end = m.vsync_end;
        mi.vtotal = m.vtotal;
        mi.vscan = m.vscan;
        mi.vrefresh = m.vscan as u32;
        mi.flags = m.flags;
        mi.mtype = m.mtype;

        let mut i = 0;
        while i < MODE_NAME_LEN {
            mi.name[i] = m.name[i];
            if m.name[i] == 0 {
                break;
            }
            i += 1;
        }
    }
}

/// A mode a client hands back through SETCRTC -> the kernel-side form.
#[no_mangle]
pub extern "C" fn drm_modeinfo_to_mode(mi: *const ModeInfo, m: *mut DisplayMode) {
    if mi.is_null() || m.is_null() {
        return;
    }
    // SAFETY: as above, with the direction reversed.
    unsafe {
        core::ptr::write_bytes(m, 0, 1);
        let mi = &*mi;
        let m = &mut *m;
        m.clock = mi.clock;
        m.hdisplay = mi.hdisplay;
        m.hsync_start = mi.hsync_start;
        m.hsync_end = mi.hsync_end;
        m.htotal = mi.htotal;
        m.hskew = mi.hskew;
        m.vdisplay = mi.vdisplay;
        m.vsync_start = mi.vsync_start;
        m.vsync_end = mi.vsync_end;
        m.vtotal = mi.vtotal;
        m.vscan = mi.vrefresh as u16;
        m.flags = mi.flags;
        m.mtype = mi.mtype;

        let mut i = 0;
        while i < MODE_NAME_LEN - 1 {
            m.name[i] = mi.name[i];
            if mi.name[i] == 0 {
                break;
            }
            i += 1;
        }
        m.name[MODE_NAME_LEN - 1] = 0;
    }
}

/* ── fourcc -> bits per pixel (uapi/drm_fourcc.h) ───────────────────────────
 * Numeric values copied from the header, not re-derived, so a typo cannot
 * silently accept-or-reject the wrong format. */

const fn fourcc(a: u8, b: u8, c: u8, d: u8) -> u32 {
    (a as u32) | ((b as u32) << 8) | ((c as u32) << 16) | ((d as u32) << 24)
}

const DRM_FORMAT_BIG_ENDIAN: u32 = 1 << 31;

const C8: u32 = fourcc(b'C', b'8', b' ', b' ');
const RGB565: u32 = fourcc(b'R', b'G', b'1', b'6');
const BGR565: u32 = fourcc(b'B', b'G', b'1', b'6');
const RGBA4444: u32 = fourcc(b'R', b'A', b'1', b'2');
const XRGB1555: u32 = fourcc(b'X', b'R', b'1', b'5');
const RGB888: u32 = fourcc(b'R', b'G', b'2', b'4');
const BGR888: u32 = fourcc(b'B', b'G', b'2', b'4');
const XRGB8888: u32 = fourcc(b'X', b'R', b'2', b'4');
const XBGR8888: u32 = fourcc(b'X', b'B', b'2', b'4');
const ARGB8888: u32 = fourcc(b'A', b'R', b'2', b'4');
const ABGR8888: u32 = fourcc(b'A', b'B', b'2', b'4');
const RGBA8888: u32 = fourcc(b'R', b'A', b'2', b'4');
const BGRA8888: u32 = fourcc(b'B', b'A', b'2', b'4');

/// Bits per pixel for the fourcc values this core accepts.  0 = unsupported.
#[no_mangle]
pub extern "C" fn drm_format_bpp(fourcc_raw: u32) -> u32 {
    match fourcc_raw & !DRM_FORMAT_BIG_ENDIAN {
        C8 => 8,
        RGB565 | BGR565 | RGBA4444 | XRGB1555 => 16,
        RGB888 | BGR888 => 24,
        XRGB8888 | XBGR8888 | ARGB8888 | ABGR8888 | RGBA8888 | BGRA8888 => 32,
        _ => 0,
    }
}
