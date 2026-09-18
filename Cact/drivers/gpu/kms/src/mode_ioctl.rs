//! The KMS ioctl table plus the resource-listing ioctls: GETRESOURCES,
//! GETCONNECTOR, GETENCODER, GETPLANERESOURCES, GETPLANE.
//!
//! `drm_mode_ioctl()` switches on the ioctl's sequence number alone; the core
//! dispatcher hands it everything in the KMS band (>= DRM_COMMAND_END) and
//! expects -ENOSYS for anything it does not own.

use alloc::vec::Vec;
use core::ffi::{c_int, c_void};

use super::atomic::mode_atomic;
use super::crtc::{mode_cursor, mode_cursor2, mode_get_crtc, mode_page_flip, mode_set_crtc};
use super::framebuffer::{mode_addfb, mode_addfb2, mode_dirtyfb, mode_getfb2, mode_rmfb};
use super::mode_object::{
    drm_connector_find, drm_encoder_find, drm_plane_find, mode_set_plane, Connector, Encoder, Plane,
};
use super::property::{
    drm_prop_collect, mode_create_blob, mode_destroy_blob, mode_get_blob, mode_get_property,
    mode_obj_get_properties, mode_obj_set_property,
};
use crate::ffi::{drm_copy_in, drm_copy_out, drm_put_raw, uptru};
use crate::gem::{drm_gem_dumb_create, drm_gem_dumb_destroy, drm_gem_dumb_map_offset};
use crate::mode::{drm_mode_to_modeinfo, ModeInfo, MODE_NAME_LEN};
use crate::structs::DrmFile;

/* ── uapi structs ───────────────────────────────────────────────────────── */

/// `struct drm_mode_card_res` — 64 bytes.
#[repr(C)]
pub struct ModeCardRes {
    pub fb_id_ptr: u64,
    pub crtc_id_ptr: u64,
    pub connector_id_ptr: u64,
    pub encoder_id_ptr: u64,
    pub count_fbs: u32,
    pub count_crtcs: u32,
    pub count_connectors: u32,
    pub count_encoders: u32,
    pub min_width: u32,
    pub max_width: u32,
    pub min_height: u32,
    pub max_height: u32,
}

/// `struct drm_mode_get_connector` — 80 bytes.
#[repr(C)]
pub struct ModeGetConnector {
    pub encoders_ptr: u64,
    pub modes_ptr: u64,
    pub props_ptr: u64,
    pub prop_values_ptr: u64,
    pub count_modes: u32,
    pub count_props: u32,
    pub count_encoders: u32,
    pub encoder_id: u32,
    pub connector_id: u32,
    pub connector_type: u32,
    pub connector_type_id: u32,
    pub connection: u32,
    pub mm_width: u32,
    pub mm_height: u32,
    pub subpixel: u32,
    pub pad: u32,
}

/// `struct drm_mode_get_encoder` — 20 bytes.
#[repr(C)]
pub struct ModeGetEncoder {
    pub encoder_id: u32,
    pub encoder_type: u32,
    pub crtc_id: u32,
    pub possible_crtcs: u32,
    pub possible_clones: u32,
}

/// `struct drm_mode_get_plane_res` — 12 bytes.
#[repr(C)]
pub struct ModeGetPlaneRes {
    pub plane_id_ptr: u64,
    pub count_planes: u32,
}

/// `struct drm_mode_get_plane` — 32 bytes.
#[repr(C)]
pub struct ModeGetPlane {
    pub plane_id: u32,
    pub crtc_id: u32,
    pub fb_id: u32,
    pub possible_crtcs: u32,
    pub gamma_size: u32,
    pub count_format_types: u32,
    pub format_type_ptr: u64,
}

const _: () = assert!(core::mem::size_of::<ModeCardRes>() == 64);
const _: () = assert!(core::mem::offset_of!(ModeCardRes, count_fbs) == 32);
const _: () = assert!(core::mem::offset_of!(ModeCardRes, count_encoders) == 44);
const _: () = assert!(core::mem::offset_of!(ModeCardRes, min_width) == 48);

const _: () = assert!(core::mem::size_of::<ModeGetConnector>() == 80);
const _: () = assert!(core::mem::offset_of!(ModeGetConnector, modes_ptr) == 8);
const _: () = assert!(core::mem::offset_of!(ModeGetConnector, count_modes) == 32);
const _: () = assert!(core::mem::offset_of!(ModeGetConnector, connector_id) == 48);
const _: () = assert!(core::mem::offset_of!(ModeGetConnector, connection) == 60);
const _: () = assert!(core::mem::offset_of!(ModeGetConnector, mm_width) == 64);

const _: () = assert!(core::mem::size_of::<ModeGetEncoder>() == 20);
const _: () = assert!(core::mem::offset_of!(ModeGetEncoder, encoder_type) == 4);
const _: () = assert!(core::mem::offset_of!(ModeGetEncoder, crtc_id) == 8);

const _: () = assert!(core::mem::size_of::<ModeGetPlaneRes>() == 12);
const _: () = assert!(core::mem::offset_of!(ModeGetPlaneRes, count_planes) == 8);

const _: () = assert!(core::mem::size_of::<ModeGetPlane>() == 32);
const _: () = assert!(core::mem::offset_of!(ModeGetPlane, possible_crtcs) == 12);
const _: () = assert!(core::mem::offset_of!(ModeGetPlane, count_format_types) == 20);
const _: () = assert!(core::mem::offset_of!(ModeGetPlane, format_type_ptr) == 24);

/* ioctl numbers (uapi/drm.h). */
const fn drm_iowr(nr: u32, size: u32) -> u32 {
    (3u32 << 30) | (0x64u32 << 8) | nr | (size << 16)
}
const DRM_IOCTL_MODE_GETRESOURCES: u32 = drm_iowr(0xA0, 64);
const DRM_IOCTL_MODE_GETENCODER: u32 = drm_iowr(0xA6, 20);
const DRM_IOCTL_MODE_GETCONNECTOR: u32 = drm_iowr(0xA7, 80);
const DRM_IOCTL_MODE_GETPLANERESOURCES: u32 = drm_iowr(0xB5, 12);
const DRM_IOCTL_MODE_GETPLANE: u32 = drm_iowr(0xB6, 32);

/* Sequence numbers the dispatcher needs; a driver-private command lives in
 * [DRM_COMMAND_BASE, DRM_COMMAND_END) and is not ours. */
const NR_GETRESOURCES: u32 = 0xA0;
const NR_GETCRTC: u32 = 0xA1;
const NR_SETCRTC: u32 = 0xA2;
const NR_CURSOR: u32 = 0xA3;
const NR_GETENCODER: u32 = 0xA6;
const NR_GETCONNECTOR: u32 = 0xA7;
const NR_GETPROPERTY: u32 = 0xAA;
const NR_GETPROPBLOB: u32 = 0xAC;
const NR_ADDFB: u32 = 0xAE;
const NR_RMFB: u32 = 0xAF;
const NR_PAGE_FLIP: u32 = 0xB0;
const NR_DIRTYFB: u32 = 0xB1;
const NR_CREATE_DUMB: u32 = 0xB2;
const NR_MAP_DUMB: u32 = 0xB3;
const NR_DESTROY_DUMB: u32 = 0xB4;
const NR_GETPLANERESOURCES: u32 = 0xB5;
const NR_GETPLANE: u32 = 0xB6;
const NR_SETPLANE: u32 = 0xB7;
const NR_ADDFB2: u32 = 0xB8;
const NR_OBJ_GETPROPERTIES: u32 = 0xB9;
const NR_OBJ_SETPROPERTY: u32 = 0xBA;
const NR_CURSOR2: u32 = 0xBB;
const NR_ATOMIC: u32 = 0xBC;
const NR_GETFB2: u32 = 0xCE;
const NR_CREATEPROPBLOB: u32 = 0xBD;
const NR_DESTROYPROPBLOB: u32 = 0xBE;

/* Syncobj commands share the same band (they are core, not KMS, but the
 * dispatcher sends everything above DRM_COMMAND_END here). */
const NR_SYNCOBJ_CREATE: u32 = 0xBF;
const NR_SYNCOBJ_DESTROY: u32 = 0xC0;
const NR_SYNCOBJ_HANDLE_TO_FD: u32 = 0xC1;
const NR_SYNCOBJ_FD_TO_HANDLE: u32 = 0xC2;
const NR_SYNCOBJ_WAIT: u32 = 0xC3;
const NR_SYNCOBJ_RESET: u32 = 0xC4;
const NR_SYNCOBJ_SIGNAL: u32 = 0xC5;
const NR_SYNCOBJ_TIMELINE_WAIT: u32 = 0xCA;
const NR_SYNCOBJ_QUERY: u32 = 0xCB;
const NR_SYNCOBJ_TIMELINE_SIGNAL: u32 = 0xCD;

const DRM_MODE_OBJECT_CONNECTOR: u32 = 0xc0c0_c0c0;

/// An all-zero `ModeInfo`, so each handler can build one without `Default`.
pub(crate) fn zero_modeinfo() -> ModeInfo {
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

/* ── GETRESOURCES ───────────────────────────────────────────────────────── */

#[no_mangle]
pub extern "C" fn mode_get_resources(file: *mut DrmFile, arg: *mut c_void) -> c_int {
    if file.is_null() {
        return -22;
    }
    let mut req = ModeCardRes {
        fb_id_ptr: 0,
        crtc_id_ptr: 0,
        connector_id_ptr: 0,
        encoder_id_ptr: 0,
        count_fbs: 0,
        count_crtcs: 0,
        count_connectors: 0,
        count_encoders: 0,
        min_width: 0,
        max_width: 0,
        min_height: 0,
        max_height: 0,
    };
    if drm_copy_in(
        &mut req as *mut _ as *mut c_void,
        DRM_IOCTL_MODE_GETRESOURCES,
        arg,
        core::mem::size_of::<ModeCardRes>() as u32,
    ) != 0
    {
        return -22;
    }

    // SAFETY: caller's file.
    let dev = unsafe { (*file).dev };
    let mut crtcs: Vec<u32> = Vec::new();
    let mut conns: Vec<u32> = Vec::new();
    let mut encs: Vec<u32> = Vec::new();
    let mut fbs: Vec<u32> = Vec::new();

    // SAFETY: read-only walk of the caller's device.
    unsafe {
        for &p in (*dev).crtcs.iter() {
            let c = p as *mut super::mode_object::Crtc;
            if !c.is_null() {
                crtcs.push((*c).id);
            }
        }
        for &p in (*dev).connectors.iter() {
            let c = p as *mut Connector;
            if !c.is_null() {
                conns.push((*c).id);
            }
        }
        for &p in (*dev).encoders.iter() {
            let e = p as *mut Encoder;
            if !e.is_null() {
                encs.push((*e).id);
            }
        }
        for &p in (*dev).fbs.iter() {
            let f = p as *mut super::framebuffer::Framebuffer;
            if !f.is_null() {
                fbs.push((*f).id);
            }
        }
    }

    if req.count_crtcs != 0
        && drm_put_raw(
            uptru(req.crtc_id_ptr),
            crtcs.as_ptr() as *const c_void,
            crtcs.len() as u32 * 4,
        ) != 0
    {
        return -22;
    }
    if req.count_connectors != 0
        && drm_put_raw(
            uptru(req.connector_id_ptr),
            conns.as_ptr() as *const c_void,
            conns.len() as u32 * 4,
        ) != 0
    {
        return -22;
    }
    if req.count_encoders != 0
        && drm_put_raw(
            uptru(req.encoder_id_ptr),
            encs.as_ptr() as *const c_void,
            encs.len() as u32 * 4,
        ) != 0
    {
        return -22;
    }
    if req.count_fbs != 0
        && drm_put_raw(uptru(req.fb_id_ptr), fbs.as_ptr() as *const c_void, fbs.len() as u32 * 4)
            != 0
    {
        return -22;
    }

    req.count_crtcs = crtcs.len() as u32;
    req.count_connectors = conns.len() as u32;
    req.count_encoders = encs.len() as u32;
    req.count_fbs = fbs.len() as u32;
    req.fb_id_ptr = 0;
    req.crtc_id_ptr = 0;
    req.connector_id_ptr = 0;
    req.encoder_id_ptr = 0;
    /* Well under any real encoder's limits, and honest: the core does not
     * enforce a size, the driver's mode list does. */
    req.min_width = 320;
    req.max_width = 8192;
    req.min_height = 200;
    req.max_height = 8192;
    drm_copy_out(arg, &req as *const _ as *const c_void, core::mem::size_of::<ModeCardRes>() as u32)
}

/* ── GETCONNECTOR ───────────────────────────────────────────────────────── */

#[no_mangle]
pub extern "C" fn mode_get_connector(file: *mut DrmFile, arg: *mut c_void) -> c_int {
    if file.is_null() {
        return -22;
    }
    let mut req = ModeGetConnector {
        encoders_ptr: 0,
        modes_ptr: 0,
        props_ptr: 0,
        prop_values_ptr: 0,
        count_modes: 0,
        count_props: 0,
        count_encoders: 0,
        encoder_id: 0,
        connector_id: 0,
        connector_type: 0,
        connector_type_id: 0,
        connection: 0,
        mm_width: 0,
        mm_height: 0,
        subpixel: 0,
        pad: 0,
    };
    if drm_copy_in(
        &mut req as *mut _ as *mut c_void,
        DRM_IOCTL_MODE_GETCONNECTOR,
        arg,
        core::mem::size_of::<ModeGetConnector>() as u32,
    ) != 0
    {
        return -22;
    }

    // SAFETY: caller's file; objects are looked up in its device.
    unsafe {
        let dev = (*file).dev;
        let c = drm_connector_find(dev, req.connector_id) as *mut Connector;
        if c.is_null() {
            return -22;
        }

        /* Everything large goes on the heap: an earlier version kept the whole
         * mode list and two 256-entry property arrays on the kernel stack,
         * overflowed it and corrupted the syscall frame.  Modes are still
         * converted one at a time. */
        let mut encs: Vec<u32> = Vec::new();
        if !(*c).encoder.is_null() {
            encs.push((*((*c).encoder as *mut Encoder)).id);
        } else {
            /* No fixed encoder binding: report all of them so userspace can
             * pick one and hand it back through SETCRTC. */
            for &p in (*dev).encoders.iter() {
                let enc = p as *mut Encoder;
                if !enc.is_null() {
                    encs.push((*enc).id);
                }
            }
        }

        let n_props = drm_prop_collect(
            dev,
            DRM_MODE_OBJECT_CONNECTOR,
            (*c).id,
            core::ptr::null_mut(),
            core::ptr::null_mut(),
            0,
        );
        let n_props = if n_props < 0 { 0 } else { n_props as usize };
        let mut prop_ids: Vec<u32> = Vec::new();
        let mut prop_vals: Vec<u64> = Vec::new();
        let mut n_props_filled = 0usize;
        if req.count_props != 0 && n_props > 0 {
            prop_ids.resize(n_props, 0);
            prop_vals.resize(n_props, 0);
            let got = drm_prop_collect(
                dev,
                DRM_MODE_OBJECT_CONNECTOR,
                (*c).id,
                prop_ids.as_mut_ptr(),
                prop_vals.as_mut_ptr(),
                n_props as c_int,
            );
            n_props_filled = if got < 0 {
                0
            } else if got as usize > n_props {
                n_props
            } else {
                got as usize
            };
        }

        let modes: &[crate::mode::DisplayMode] = &(*c).modes;
        if req.count_modes != 0 && req.modes_ptr != 0 && !modes.is_empty() {
            let n = if (req.count_modes as usize) < modes.len() {
                req.count_modes as usize
            } else {
                modes.len()
            };
            for i in 0..n {
                let mut mi = zero_modeinfo();
                drm_mode_to_modeinfo(&modes[i], &mut mi);
                let user = uptru(req.modes_ptr + (i * core::mem::size_of::<ModeInfo>()) as u64);
                if drm_put_raw(
                    user,
                    &mi as *const _ as *const c_void,
                    core::mem::size_of::<ModeInfo>() as u32,
                ) != 0
                {
                    return -22;
                }
            }
        }
        if req.count_encoders != 0
            && drm_put_raw(
                uptru(req.encoders_ptr),
                encs.as_ptr() as *const c_void,
                encs.len() as u32 * 4,
            ) != 0
        {
            return -22;
        }

        if req.count_props != 0 && n_props_filled > 0 {
            if drm_put_raw(
                uptru(req.props_ptr),
                prop_ids.as_ptr() as *const c_void,
                n_props_filled as u32 * 4,
            ) != 0
            {
                return -22;
            }
            if drm_put_raw(
                uptru(req.prop_values_ptr),
                prop_vals.as_ptr() as *const c_void,
                n_props_filled as u32 * 8,
            ) != 0
            {
                return -22;
            }
        }

        req.encoder_id = if (*c).encoder.is_null() {
            0
        } else {
            (*((*c).encoder as *mut Encoder)).id
        };
        req.connector_type = (*c).connector_type;
        req.connector_type_id = (*c).connector_type_id;
        req.count_modes = (*c).modes.len() as u32;
        req.count_props = n_props as u32;
        req.count_encoders = encs.len() as u32;
        req.mm_width = (*c).mm_width;
        req.mm_height = (*c).mm_height;
        req.connection = (*c).status;
        req.subpixel = 0;
        req.modes_ptr = 0;
        req.props_ptr = 0;
        req.prop_values_ptr = 0;
        req.encoders_ptr = 0;
    }
    drm_copy_out(
        arg,
        &req as *const _ as *const c_void,
        core::mem::size_of::<ModeGetConnector>() as u32,
    )
}

/* ── GETENCODER ─────────────────────────────────────────────────────────── */

#[no_mangle]
pub extern "C" fn mode_get_encoder(file: *mut DrmFile, arg: *mut c_void) -> c_int {
    if file.is_null() {
        return -22;
    }
    let mut req = ModeGetEncoder {
        encoder_id: 0,
        encoder_type: 0,
        crtc_id: 0,
        possible_crtcs: 0,
        possible_clones: 0,
    };
    if drm_copy_in(
        &mut req as *mut _ as *mut c_void,
        DRM_IOCTL_MODE_GETENCODER,
        arg,
        core::mem::size_of::<ModeGetEncoder>() as u32,
    ) != 0
    {
        return -22;
    }

    // SAFETY: caller's file; the encoder is looked up in its device.
    unsafe {
        let e = drm_encoder_find((*file).dev, req.encoder_id) as *mut Encoder;
        if e.is_null() {
            return -22;
        }
        req.encoder_type = (*e).encoder_type;
        req.crtc_id = if (*e).crtc.is_null() {
            0
        } else {
            (*((*e).crtc as *mut super::mode_object::Crtc)).id
        };
        req.possible_crtcs = (*e).possible_crtcs;
        req.possible_clones = (*e).possible_clones;
    }
    drm_copy_out(arg, &req as *const _ as *const c_void, core::mem::size_of::<ModeGetEncoder>() as u32)
}

/* ── GETPLANERESOURCES ──────────────────────────────────────────────────── */

#[no_mangle]
pub extern "C" fn mode_get_plane_resources(file: *mut DrmFile, arg: *mut c_void) -> c_int {
    if file.is_null() {
        return -22;
    }
    let mut req = ModeGetPlaneRes {
        plane_id_ptr: 0,
        count_planes: 0,
    };
    if drm_copy_in(
        &mut req as *mut _ as *mut c_void,
        DRM_IOCTL_MODE_GETPLANERESOURCES,
        arg,
        core::mem::size_of::<ModeGetPlaneRes>() as u32,
    ) != 0
    {
        return -22;
    }

    // SAFETY: caller's file.
    let dev = unsafe { (*file).dev };
    let mut ids: Vec<u32> = Vec::new();
    // SAFETY: read-only walk of the caller's device.
    unsafe {
        for &p in (*dev).planes.iter() {
            let pl = p as *mut Plane;
            if !pl.is_null() {
                ids.push((*pl).id);
            }
        }
    }

    if req.count_planes != 0
        && drm_put_raw(uptru(req.plane_id_ptr), ids.as_ptr() as *const c_void, ids.len() as u32 * 4)
            != 0
    {
        return -22;
    }
    req.count_planes = ids.len() as u32;
    req.plane_id_ptr = 0;
    drm_copy_out(
        arg,
        &req as *const _ as *const c_void,
        core::mem::size_of::<ModeGetPlaneRes>() as u32,
    )
}

/* ── GETPLANE ───────────────────────────────────────────────────────────── */

#[no_mangle]
pub extern "C" fn mode_get_plane(file: *mut DrmFile, arg: *mut c_void) -> c_int {
    if file.is_null() {
        return -22;
    }
    let mut req = ModeGetPlane {
        plane_id: 0,
        crtc_id: 0,
        fb_id: 0,
        possible_crtcs: 0,
        gamma_size: 0,
        count_format_types: 0,
        format_type_ptr: 0,
    };
    if drm_copy_in(
        &mut req as *mut _ as *mut c_void,
        DRM_IOCTL_MODE_GETPLANE,
        arg,
        core::mem::size_of::<ModeGetPlane>() as u32,
    ) != 0
    {
        return -22;
    }

    // SAFETY: caller's file; the plane is looked up in its device.
    unsafe {
        let p = drm_plane_find((*file).dev, req.plane_id) as *mut Plane;
        if p.is_null() {
            return -22;
        }

        if req.count_format_types != 0
            && drm_put_raw(
                uptru(req.format_type_ptr),
                (*p).formats.as_ptr() as *const c_void,
                (*p).formats.len() as u32 * 4,
            ) != 0
        {
            return -22;
        }

        /* What the plane is attached to, as recorded by SETPLANE / an atomic
         * commit. */
        req.crtc_id = (*p).crtc_id;
        req.fb_id = (*p).fb_id;
        req.possible_crtcs = (*p).possible_crtcs;
        req.gamma_size = 0;
        req.count_format_types = (*p).formats.len() as u32;
        req.format_type_ptr = 0;
    }
    drm_copy_out(arg, &req as *const _ as *const c_void, core::mem::size_of::<ModeGetPlane>() as u32)
}

/* ── the table ──────────────────────────────────────────────────────────── */

/// `drm_mode_ioctl()` — dispatch inside the KMS band.  Returns -ENOSYS for
/// anything the KMS layer does not own, which is what lets the core fall
/// through to the driver's private range.
#[no_mangle]
pub extern "C" fn drm_mode_ioctl(file: *mut DrmFile, nr: u32, arg: *mut c_void) -> c_int {
    match nr {
        NR_GETRESOURCES => mode_get_resources(file, arg),
        NR_GETCONNECTOR => mode_get_connector(file, arg),
        NR_GETENCODER => mode_get_encoder(file, arg),
        NR_GETCRTC => mode_get_crtc(file, arg),
        NR_SETCRTC => mode_set_crtc(file, arg),
        NR_CURSOR => mode_cursor(file, arg),
        NR_CURSOR2 => mode_cursor2(file, arg),
        NR_ATOMIC => mode_atomic(file, arg),
        NR_GETPLANERESOURCES => mode_get_plane_resources(file, arg),
        NR_GETPLANE => mode_get_plane(file, arg),
        NR_SETPLANE => mode_set_plane(file, arg),
        NR_ADDFB => mode_addfb(file, arg),
        NR_ADDFB2 => mode_addfb2(file, arg),
        NR_RMFB => mode_rmfb(file, arg),
        NR_GETFB2 => mode_getfb2(file, arg),
        NR_DIRTYFB => mode_dirtyfb(file, arg),
        NR_PAGE_FLIP => mode_page_flip(file, arg),
        NR_GETPROPERTY => mode_get_property(file, arg),
        NR_GETPROPBLOB => mode_get_blob(file, arg),
        NR_CREATEPROPBLOB => mode_create_blob(file, arg),
        NR_DESTROYPROPBLOB => mode_destroy_blob(file, arg),
        NR_OBJ_GETPROPERTIES => mode_obj_get_properties(file, arg),
        NR_OBJ_SETPROPERTY => mode_obj_set_property(file, arg),
        NR_CREATE_DUMB => drm_gem_dumb_create(file, arg),
        NR_MAP_DUMB => drm_gem_dumb_map_offset(file, arg),
        NR_DESTROY_DUMB => drm_gem_dumb_destroy(file, arg),
        NR_SYNCOBJ_CREATE => crate::syncobj::syncobj_ioctl_create(file, arg),
        NR_SYNCOBJ_DESTROY => crate::syncobj::syncobj_ioctl_destroy(file, arg),
        NR_SYNCOBJ_HANDLE_TO_FD => crate::syncobj::syncobj_ioctl_handle_to_fd(file, arg),
        NR_SYNCOBJ_FD_TO_HANDLE => crate::syncobj::syncobj_ioctl_fd_to_handle(file, arg),
        NR_SYNCOBJ_WAIT => crate::syncobj::syncobj_ioctl_wait(file, arg),
        NR_SYNCOBJ_RESET => crate::syncobj::syncobj_ioctl_reset(file, arg),
        NR_SYNCOBJ_SIGNAL => crate::syncobj::syncobj_ioctl_signal(file, arg),
        NR_SYNCOBJ_TIMELINE_WAIT => crate::syncobj::syncobj_ioctl_timeline_wait(file, arg),
        NR_SYNCOBJ_QUERY => crate::syncobj::syncobj_ioctl_query(file, arg),
        NR_SYNCOBJ_TIMELINE_SIGNAL => crate::syncobj::syncobj_ioctl_timeline_signal(file, arg),
        _ => -38,
    }
}
