//! Properties: the device-wide list, the attachment table, and the
//! GETPROPERTY / OBJ_GETPROPERTIES / OBJ_SETPROPERTY ioctls.
//!
//! Both the property list and its attachment table are allocator-backed, so a
//! driver may define as many properties as it likes and an object may carry as
//! many as it likes.  Connector DPMS is the one property with a hardware
//! effect: setting it blanks (or un-blanks) through the driver's `set_config`.

use alloc::vec;
use alloc::vec::Vec;
use core::ffi::{c_int, c_void};

use super::mode_object::{drm_connector_find, Connector, Crtc, Encoder};
use crate::device::drm_driver_set_config;
use crate::ffi::{drm_copy_in, drm_copy_out, drm_put_raw, uptru};
use crate::mode::MODE_NAME_LEN;
use crate::structs::{DrmDevice, DrmFile, ModeSet, PropAttach, Property, PROPERTY_NAME_LEN};

/* ── uapi structs (uapi/drm_mode.h) ──────────────────────────────────────── */

/// `struct drm_mode_get_property` — 64 bytes.
#[repr(C)]
pub struct ModeGetProperty {
    pub values_ptr: u64,
    pub enum_blob_ptr: u64,
    pub prop_id: u32,
    pub flags: u32,
    pub name: [u8; MODE_NAME_LEN],
    pub count_values: u32,
    pub count_enum_blobs: u32,
}

/// `struct drm_mode_property_enum` — 40 bytes.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct ModePropertyEnum {
    pub value: u64,
    pub name: [u8; MODE_NAME_LEN],
}

/// `struct drm_mode_obj_get_properties` — 28 bytes.
#[repr(C)]
pub struct ModeObjGetProperties {
    pub props_ptr: u64,
    pub prop_values_ptr: u64,
    pub count_props: u32,
    pub obj_id: u32,
    pub obj_type: u32,
}

/// `struct drm_mode_obj_set_property` — 20 bytes.
#[repr(C)]
pub struct ModeObjSetProperty {
    pub value: u64,
    pub prop_id: u32,
    pub obj_id: u32,
    pub obj_type: u32,
}

/// `struct drm_mode_get_blob` — 16 bytes.
#[repr(C)]
pub struct ModeGetBlob {
    pub blob_id: u32,
    pub length: u32,
    pub data: u64,
}

/// `struct drm_mode_create_blob` — 16 bytes.
#[repr(C)]
pub struct ModeCreateBlob {
    pub data: u64,
    pub length: u32,
    pub blob_id: u32,
}

/// `struct drm_mode_destroy_blob` — 4 bytes.
#[repr(C)]
pub struct ModeDestroyBlob {
    pub blob_id: u32,
}

const _: () = assert!(core::mem::size_of::<ModeGetBlob>() == 16);
const _: () = assert!(core::mem::offset_of!(ModeGetBlob, length) == 4);
const _: () = assert!(core::mem::size_of::<ModeCreateBlob>() == 16);
const _: () = assert!(core::mem::offset_of!(ModeCreateBlob, length) == 8);
const _: () = assert!(core::mem::size_of::<ModeDestroyBlob>() == 4);

const _: () = assert!(core::mem::size_of::<ModeGetProperty>() == 64);
const _: () = assert!(core::mem::offset_of!(ModeGetProperty, prop_id) == 16);
const _: () = assert!(core::mem::offset_of!(ModeGetProperty, name) == 24);
const _: () = assert!(core::mem::offset_of!(ModeGetProperty, count_values) == 56);
const _: () = assert!(core::mem::offset_of!(ModeGetProperty, count_enum_blobs) == 60);
const _: () = assert!(core::mem::size_of::<ModePropertyEnum>() == 40);
const _: () = assert!(core::mem::offset_of!(ModePropertyEnum, name) == 8);
const _: () = assert!(core::mem::size_of::<ModeObjGetProperties>() == 28);
const _: () = assert!(core::mem::offset_of!(ModeObjGetProperties, count_props) == 16);
const _: () = assert!(core::mem::size_of::<ModeObjSetProperty>() == 20);

/* uapi values (uapi/drm_mode.h). */
const DRM_MODE_PROP_IMMUTABLE: u32 = 1 << 2;
const DRM_MODE_PROP_ENUM: u32 = 1 << 3;
const DRM_MODE_PROP_BLOB: u32 = 1 << 4;
const DRM_MODE_PROP_BITMASK: u32 = 1 << 5;
const DRM_MODE_PROP_RANGE: u32 = 1 << 1;
const DRM_MODE_PROP_OBJECT: u32 = 1 << 6;
const DRM_MODE_PROP_SIGNED_RANGE: u32 = 2 << 6;
const DRM_MODE_PROP_ATOMIC: u32 = 0x8000_0000;
const DRM_MODE_OBJECT_CRTC: u32 = 0xcccc_cccc;
const DRM_MODE_OBJECT_CONNECTOR: u32 = 0xc0c0_c0c0;
const DRM_MODE_OBJECT_PLANE: u32 = 0xeeee_eeee;
const DRM_MODE_DPMS_ON: u64 = 0;

/// `DRM_IOWR(nr, size)`, spelled out so the dispatcher's size check works
/// (type 'd' = 0x64, direction read|write).
const fn drm_iowr(nr: u32, size: u32) -> u32 {
    (3u32 << 30) | (0x64u32 << 8) | nr | (size << 16)
}
const DRM_IOCTL_MODE_GETPROPERTY: u32 = drm_iowr(0xAA, 64);
const DRM_IOCTL_MODE_OBJ_GETPROPERTIES: u32 = drm_iowr(0xB9, 28);
const DRM_IOCTL_MODE_OBJ_SETPROPERTY: u32 = drm_iowr(0xBA, 20);
const DRM_IOCTL_MODE_GETPROPBLOB: u32 = drm_iowr(0xAC, 16);
const DRM_IOCTL_MODE_CREATEPROPBLOB: u32 = drm_iowr(0xBD, 16);
const DRM_IOCTL_MODE_DESTROYPROPBLOB: u32 = drm_iowr(0xBE, 4);

/* A single blob request is bounded so a client cannot ask the kernel for an
 * arbitrarily large allocation in one ioctl; blob storage itself is not a
 * pool and has no entry cap. */
const MAX_BLOB_BYTES: u32 = 1 << 20;

extern "C" {
    fn validate_user_ptr(ptr: *const c_void, size: u32) -> c_int;
    fn copy_from_user(dst: *mut c_void, src: *const c_void, size: u32) -> c_int;
    fn copy_to_user(dst: *mut c_void, src: *const c_void, size: u32) -> c_int;
}

/* ── the pool ────────────────────────────────────────────────────────────── */

/// Gather the properties attached to one object.
///
/// Writes at most `max` entries and returns the number of attachments that
/// exist, so a caller can count with `max == 0` first and then ask for exactly
/// that many.
#[no_mangle]
pub extern "C" fn drm_prop_collect(
    dev: *mut DrmDevice,
    obj_type: u32,
    obj_id: u32,
    ids: *mut u32,
    values: *mut u64,
    max: c_int,
) -> c_int {
    if dev.is_null() {
        return 0;
    }
    let limit = if max <= 0 { 0usize } else { max as usize };
    let mut total = 0usize;
    let mut filled = 0usize;
    // SAFETY: caller's device; writes stay under `limit` and only happen when
    // the caller passed the corresponding array.
    unsafe {
        for a in &(*dev).prop_attach {
            if a.obj_type == obj_type && a.obj_id == obj_id {
                if filled < limit && !ids.is_null() && !values.is_null() {
                    *ids.add(filled) = a.prop_id;
                    *values.add(filled) = a.value;
                    filled += 1;
                }
                total += 1;
            }
        }
    }
    total as c_int
}

/// `drm_prop_find` — look a property up by id.
///
/// The pointer borrows the device's own list; callers must not create or remove
/// properties while holding it.
#[no_mangle]
pub extern "C" fn drm_prop_find(dev: *mut DrmDevice, id: u32) -> *mut c_void {
    if dev.is_null() {
        return core::ptr::null_mut();
    }
    // SAFETY: read-only walk of the caller's device.
    unsafe { (*dev).find_prop(id) as *mut c_void }
}

/// `drm_prop_create` — define a new device property.
#[no_mangle]
pub extern "C" fn drm_prop_create(
    dev: *mut DrmDevice,
    name: *const u8,
    flags: u32,
    ptype: u32,
    num_values: c_int,
    values: *const u64,
    enum_names: *const *const u8,
    val_len: u32,
) -> u32 {
    if dev.is_null() || name.is_null() {
        return 0;
    }
    // SAFETY: caller's device and name; the argument arrays are read for
    // exactly `num_values` entries when non-null.
    unsafe {
        let mut pname = [0u8; PROPERTY_NAME_LEN];
        let mut k = 0;
        while k < PROPERTY_NAME_LEN - 1 {
            let b = *name.add(k);
            pname[k] = b;
            if b == 0 {
                break;
            }
            k += 1;
        }

        let n = if num_values > 0 { num_values as usize } else { 0 };
        let mut vals: Vec<u64> = Vec::new();
        let mut names: Vec<*const u8> = Vec::new();
        if n > 0 {
            vals.reserve(n);
            names.reserve(n);
            for v in 0..n {
                vals.push(if values.is_null() { 0 } else { *values.add(v) });
                names.push(if enum_names.is_null() {
                    core::ptr::null()
                } else {
                    *enum_names.add(v)
                });
            }
        }

        let dev = &mut *dev;
        dev.next_prop_id += 1;
        let id = dev.next_prop_id;
        dev.props.push(Property {
            id,
            name: pname,
            flags,
            ptype,
            values: vals,
            enum_names: names,
            val_len: if val_len != 0 { val_len } else { 4 },
        });
        id
    }
}

/// `drm_prop_attach` — attach a property to an object (idempotent).
#[no_mangle]
pub extern "C" fn drm_prop_attach(
    dev: *mut DrmDevice,
    prop_id: u32,
    obj_type: u32,
    obj_id: u32,
    value: u64,
) -> c_int {
    if dev.is_null() || drm_prop_find(dev, prop_id).is_null() {
        return -22;
    }
    // SAFETY: caller's device.
    unsafe {
        let dev = &mut *dev;
        for a in &mut dev.prop_attach {
            if a.prop_id == prop_id && a.obj_type == obj_type && a.obj_id == obj_id {
                a.value = value;
                return 0;
            }
        }
        dev.prop_attach.push(PropAttach {
            prop_id,
            obj_type,
            obj_id,
            value,
        });
    }
    0
}

/// `drm_prop_set` — change the value of an existing attachment.
#[no_mangle]
pub extern "C" fn drm_prop_set(
    dev: *mut DrmDevice,
    prop_id: u32,
    obj_type: u32,
    obj_id: u32,
    value: u64,
) -> c_int {
    if dev.is_null() {
        return -22;
    }
    // SAFETY: caller's device.
    unsafe {
        let dev = &mut *dev;
        for a in &mut dev.prop_attach {
            if a.prop_id == prop_id && a.obj_type == obj_type && a.obj_id == obj_id {
                a.value = value;
                return 0;
            }
        }
    }
    -22
}

/// Copy a NUL-terminated property name into a uapi name field.
fn copy_prop_name(dst: &mut [u8; MODE_NAME_LEN], src: &[u8; PROPERTY_NAME_LEN]) {
    let mut k = 0;
    while k < MODE_NAME_LEN - 1 {
        dst[k] = src[k];
        if src[k] == 0 {
            return;
        }
        k += 1;
    }
    dst[MODE_NAME_LEN - 1] = 0;
}

/* ── ioctls ──────────────────────────────────────────────────────────────── */

/// `DRM_IOCTL_MODE_GETPROPERTY`.
#[no_mangle]
pub extern "C" fn mode_get_property(file: *mut DrmFile, arg: *mut c_void) -> c_int {
    if file.is_null() {
        return -22;
    }
    let mut req = ModeGetProperty {
        values_ptr: 0,
        enum_blob_ptr: 0,
        prop_id: 0,
        flags: 0,
        name: [0; MODE_NAME_LEN],
        count_values: 0,
        count_enum_blobs: 0,
    };
    if drm_copy_in(
        &mut req as *mut _ as *mut c_void,
        DRM_IOCTL_MODE_GETPROPERTY,
        arg,
        core::mem::size_of::<ModeGetProperty>() as u32,
    ) != 0
    {
        return -22;
    }

    // SAFETY: `file` is the caller's open client.
    let dev = unsafe { (*file).dev };
    let p = drm_prop_find(dev, req.prop_id) as *const Property;
    if p.is_null() {
        return -22;
    }
    // SAFETY: `p` was just looked up in the caller's device, and nothing in
    // this handler adds or removes a property.
    let p = unsafe { &*p };

    if req.count_values != 0 && req.values_ptr != 0 && !p.values.is_empty() {
        if drm_put_raw(
            uptru(req.values_ptr),
            p.values.as_ptr() as *const c_void,
            p.values.len() as u32 * 8,
        ) != 0
        {
            return -22;
        }
    }

    /* ENUM text: userspace asks for count_enum_blobs first, then a run of
     * drm_mode_property_enum {value, name[32]} entries. */
    if p.flags & (DRM_MODE_PROP_ENUM | DRM_MODE_PROP_BITMASK) != 0 {
        if req.count_enum_blobs != 0 && req.enum_blob_ptr != 0 {
            let n = p.values.len();
            let mut en: Vec<ModePropertyEnum> = vec![
                ModePropertyEnum {
                    value: 0,
                    name: [0; MODE_NAME_LEN],
                };
                n
            ];
            for i in 0..n {
                en[i].value = p.values[i];
                let src = p.enum_names[i];
                if !src.is_null() {
                    // SAFETY: the pool stores NUL-terminated names.
                    unsafe {
                        let mut k = 0;
                        while k < MODE_NAME_LEN - 1 {
                            let b = *src.add(k);
                            en[i].name[k] = b;
                            if b == 0 {
                                break;
                            }
                            k += 1;
                        }
                    }
                }
            }
            if drm_put_raw(
                uptru(req.enum_blob_ptr),
                en.as_ptr() as *const c_void,
                n as u32 * core::mem::size_of::<ModePropertyEnum>() as u32,
            ) != 0
            {
                return -22;
            }
        }
        req.count_enum_blobs = p.values.len() as u32;
    } else {
        req.count_enum_blobs = 0;
        req.enum_blob_ptr = 0;
    }

    req.flags = p.flags;
    req.count_values = p.values.len() as u32;
    req.values_ptr = 0;
    if req.count_enum_blobs == 0 {
        req.enum_blob_ptr = 0;
    }
    copy_prop_name(&mut req.name, &p.name);
    drm_copy_out(
        arg,
        &req as *const _ as *const c_void,
        core::mem::size_of::<ModeGetProperty>() as u32,
    )
}

/// `DRM_IOCTL_MODE_OBJ_GETPROPERTIES`.
#[no_mangle]
pub extern "C" fn mode_obj_get_properties(file: *mut DrmFile, arg: *mut c_void) -> c_int {
    if file.is_null() {
        return -22;
    }
    let mut req = ModeObjGetProperties {
        props_ptr: 0,
        prop_values_ptr: 0,
        count_props: 0,
        obj_id: 0,
        obj_type: 0,
    };
    if drm_copy_in(
        &mut req as *mut _ as *mut c_void,
        DRM_IOCTL_MODE_OBJ_GETPROPERTIES,
        arg,
        core::mem::size_of::<ModeObjGetProperties>() as u32,
    ) != 0
    {
        return -22;
    }

    /* Count first, then materialise exactly that many on the heap: a 256-entry
     * table on the kernel stack is what this handler must not spend, and the
     * attachment table is no longer a fixed size anyway. */
    // SAFETY: `file` is the caller's open client.
    let dev = unsafe { (*file).dev };
    let n = drm_prop_collect(dev, req.obj_type, req.obj_id, core::ptr::null_mut(), core::ptr::null_mut(), 0);
    if n < 0 {
        return -22;
    }
    let n = n as usize;

    if req.count_props != 0 && n > 0 {
        let mut ids: Vec<u32> = Vec::new();
        let mut vals: Vec<u64> = Vec::new();
        ids.resize(n, 0);
        vals.resize(n, 0);
        let filled = drm_prop_collect(
            dev,
            req.obj_type,
            req.obj_id,
            ids.as_mut_ptr(),
            vals.as_mut_ptr(),
            n as c_int,
        );
        let filled = if filled < 0 {
            0
        } else if filled as usize > n {
            n
        } else {
            filled as usize
        };
        if drm_put_raw(uptru(req.props_ptr), ids.as_ptr() as *const c_void, filled as u32 * 4) != 0 {
            return -22;
        }
        if drm_put_raw(
            uptru(req.prop_values_ptr),
            vals.as_ptr() as *const c_void,
            filled as u32 * 8,
        ) != 0
        {
            return -22;
        }
    }

    req.count_props = n as u32;
    req.props_ptr = 0;
    req.prop_values_ptr = 0;
    drm_copy_out(
        arg,
        &req as *const _ as *const c_void,
        core::mem::size_of::<ModeObjGetProperties>() as u32,
    )
}

/// `DRM_IOCTL_MODE_OBJ_SETPROPERTY`.
#[no_mangle]
pub extern "C" fn mode_obj_set_property(file: *mut DrmFile, arg: *mut c_void) -> c_int {
    if file.is_null() {
        return -22;
    }
    let mut req = ModeObjSetProperty {
        value: 0,
        prop_id: 0,
        obj_id: 0,
        obj_type: 0,
    };
    if drm_copy_in(
        &mut req as *mut _ as *mut c_void,
        DRM_IOCTL_MODE_OBJ_SETPROPERTY,
        arg,
        core::mem::size_of::<ModeObjSetProperty>() as u32,
    ) != 0
    {
        return -22;
    }

    // SAFETY: `file` is the caller's open client.
    let dev = unsafe { (*file).dev };
    let p = drm_prop_find(dev, req.prop_id) as *const Property;
    if p.is_null() {
        return -22;
    }

    let rc = drm_prop_set(dev, req.prop_id, req.obj_type, req.obj_id, req.value);
    if rc != 0 {
        return rc;
    }

    /* DPMS on a connector is the one property with a hardware effect: the
     * driver hears about it through set_config (blanking). */
    // SAFETY: `p` was just looked up; drm_prop_set does not add or remove.
    let is_dpms = unsafe {
        let prop: &Property = &*p;
        prop.name[0..5] == *b"DPMS\0"
    };
    if req.obj_type == DRM_MODE_OBJECT_CONNECTOR && is_dpms {
        let c = drm_connector_find(dev, req.obj_id) as *mut Connector;
        if c.is_null() {
            return -22;
        }
        // SAFETY: connector looked up in the caller's device.
        unsafe {
            (*c).dpms = req.value as u32;
            let enc = (*c).encoder;
            if !enc.is_null() {
                let crtc = (*(enc as *mut Encoder)).crtc;
                if !crtc.is_null() {
                    let crtc = crtc as *mut Crtc;
                    let on = req.value == DRM_MODE_DPMS_ON;
                    let mut conns: [*mut c_void; 1] = [c as *mut c_void];
                    let mut set = ModeSet {
                        fb: if on { (*crtc).fb } else { core::ptr::null_mut() },
                        crtc: crtc as *mut c_void,
                        mode: (*crtc).mode,
                        x: 0,
                        y: 0,
                        connectors: conns.as_mut_ptr(),
                        num_connectors: 1,
                    };
                    let src = drm_driver_set_config(dev, &mut set);
                    if src != 0 {
                        return src;
                    }
                    (*crtc).enabled = if on { 1 } else { 0 };
                }
            }
        }
    }
    0
}

/* ── blob properties ────────────────────────────────────────────────────── */

/// The connector `EDID` property: one device-wide, immutable blob property that
/// every connector attaches (with blob id 0 until an EDID is set).
///
/// This is how libdrm reads the EDID — `drm_mode_get_connector` has no EDID
/// field, so the client gets the blob id from OBJ_GETPROPERTIES and the bytes
/// from GET_BLOB.
pub(crate) fn drm_prop_edid_id(dev: *mut DrmDevice) -> u32 {
    if dev.is_null() {
        return 0;
    }
    // SAFETY: caller's device.
    unsafe {
        let existing = (*dev).find_prop_by_name(b"EDID");
        if existing != 0 {
            return existing;
        }
        drm_prop_create(
            dev,
            b"EDID\0".as_ptr(),
            DRM_MODE_PROP_BLOB | DRM_MODE_PROP_IMMUTABLE,
            DRM_MODE_PROP_BLOB,
            0,
            core::ptr::null(),
            core::ptr::null(),
            4,
        )
    }
}

/// The universal-planes `type` property: one device-wide, immutable enum
/// property shared by every plane (Overlay / Primary / Cursor).
pub(crate) fn drm_prop_plane_type_id(dev: *mut DrmDevice) -> u32 {
    static OVERLAY: &[u8] = b"Overlay\0";
    static PRIMARY: &[u8] = b"Primary\0";
    static CURSOR: &[u8] = b"Cursor\0";
    if dev.is_null() {
        return 0;
    }
    // SAFETY: caller's device.
    unsafe {
        let existing = (*dev).find_prop_by_name(b"type");
        if existing != 0 {
            return existing;
        }
        let values: [u64; 3] = [0, 1, 2];
        let names: [*const u8; 3] = [OVERLAY.as_ptr(), PRIMARY.as_ptr(), CURSOR.as_ptr()];
        drm_prop_create(
            dev,
            b"type\0".as_ptr(),
            DRM_MODE_PROP_ENUM | DRM_MODE_PROP_IMMUTABLE,
            DRM_MODE_PROP_ENUM,
            3,
            values.as_ptr(),
            names.as_ptr(),
            4,
        )
    }
}

/// `DRM_IOCTL_MODE_CREATEPROPBLOB` — copy client bytes into a new blob.  Used
/// for the `MODE_ID` blobs an atomic commit refers to.
#[no_mangle]
pub extern "C" fn mode_create_blob(file: *mut DrmFile, arg: *mut c_void) -> c_int {
    if file.is_null() {
        return -22;
    }
    let mut req = ModeCreateBlob {
        data: 0,
        length: 0,
        blob_id: 0,
    };
    if drm_copy_in(
        &mut req as *mut _ as *mut c_void,
        DRM_IOCTL_MODE_CREATEPROPBLOB,
        arg,
        core::mem::size_of::<ModeCreateBlob>() as u32,
    ) != 0
    {
        return -22;
    }
    if req.length == 0 || req.length > MAX_BLOB_BYTES || req.data == 0 {
        return -22;
    }

    let user = req.data as u32 as usize as *const c_void;
    let mut buf: Vec<u8> = Vec::new();
    buf.resize(req.length as usize, 0);
    // SAFETY: `user` is the client's pointer, range-checked before the copy.
    unsafe {
        if validate_user_ptr(user, req.length) == 0 {
            return -22;
        }
        if copy_from_user(buf.as_mut_ptr() as *mut c_void, user, req.length) != 0 {
            return -22;
        }
        let dev = (*file).dev;
        if dev.is_null() {
            return -22;
        }
        req.blob_id = (*dev).blob_create(&buf);
    }
    req.data = 0;
    drm_copy_out(
        arg,
        &req as *const _ as *const c_void,
        core::mem::size_of::<ModeCreateBlob>() as u32,
    )
}

/// `DRM_IOCTL_MODE_GETPROPBLOB` — the client asks with the length it expects
/// (having learned it from the property), exactly as Linux requires.
#[no_mangle]
pub extern "C" fn mode_get_blob(file: *mut DrmFile, arg: *mut c_void) -> c_int {
    if file.is_null() {
        return -22;
    }
    let mut req = ModeGetBlob {
        blob_id: 0,
        length: 0,
        data: 0,
    };
    if drm_copy_in(
        &mut req as *mut _ as *mut c_void,
        DRM_IOCTL_MODE_GETPROPBLOB,
        arg,
        core::mem::size_of::<ModeGetBlob>() as u32,
    ) != 0
    {
        return -22;
    }

    // SAFETY: caller's file; the blob is looked up in its device.
    unsafe {
        let dev = (*file).dev;
        if dev.is_null() {
            return -22;
        }
        let blob = (*dev).find_blob(req.blob_id);
        if blob.is_null() {
            return -2; // -ENOENT
        }
        let blob = &*blob;
        if req.length == blob.data.len() as u32 && req.data != 0 {
            let user = req.data as u32 as usize as *mut c_void;
            if copy_to_user(user, blob.data.as_ptr() as *const c_void, blob.data.len() as u32) != 0 {
                return -14; // -EFAULT
            }
        }
        req.length = blob.data.len() as u32;
    }
    drm_copy_out(
        arg,
        &req as *const _ as *const c_void,
        core::mem::size_of::<ModeGetBlob>() as u32,
    )
}

/// `DRM_IOCTL_MODE_DESTROYPROPBLOB`.
#[no_mangle]
pub extern "C" fn mode_destroy_blob(file: *mut DrmFile, arg: *mut c_void) -> c_int {
    if file.is_null() {
        return -22;
    }
    let mut req = ModeDestroyBlob { blob_id: 0 };
    if drm_copy_in(
        &mut req as *mut _ as *mut c_void,
        DRM_IOCTL_MODE_DESTROYPROPBLOB,
        arg,
        core::mem::size_of::<ModeDestroyBlob>() as u32,
    ) != 0
    {
        return -22;
    }
    // SAFETY: caller's file.
    unsafe {
        let dev = (*file).dev;
        if dev.is_null() {
            return -22;
        }
        if !(*dev).blob_destroy(req.blob_id) {
            return -2; // -ENOENT
        }
    }
    0
}

/* ── the core's atomic properties ───────────────────────────────────────── */

/// Get or create one of the core's own device-wide properties.  The lookup is
/// by (name, object type), so a connector's `CRTC_ID` and a plane's `CRTC_ID`
/// are separate property objects — which is what lets the atomic ioctl tell
/// which object pool an id refers to.
unsafe fn prop_get_or_create(
    dev: *mut DrmDevice,
    name: &[u8],
    flags: u32,
    ptype: u32,
    values: &[u64],
    val_len: u32,
) -> u32 {
    if dev.is_null() {
        return 0;
    }
    let existing = (*dev).find_prop_by_name_type(name, ptype);
    if existing != 0 {
        return existing;
    }
    let mut buf = [0u8; PROPERTY_NAME_LEN];
    let n = core::cmp::min(name.len(), PROPERTY_NAME_LEN - 1);
    buf[..n].copy_from_slice(&name[..n]);
    drm_prop_create(
        dev,
        buf.as_ptr(),
        flags,
        ptype,
        values.len() as c_int,
        if values.is_empty() {
            core::ptr::null()
        } else {
            values.as_ptr()
        },
        core::ptr::null(),
        val_len,
    )
}

/// `ACTIVE` on a CRTC: 0 is off, 1 is on.
pub(crate) fn drm_prop_crtc_active(dev: *mut DrmDevice) -> u32 {
    // SAFETY: caller's device.
    unsafe {
        prop_get_or_create(
            dev,
            b"ACTIVE",
            DRM_MODE_PROP_RANGE | DRM_MODE_PROP_ATOMIC,
            DRM_MODE_OBJECT_CRTC,
            &[0, 1],
            4,
        )
    }
}

/// `MODE_ID` on a CRTC: a blob id naming the mode to set.
pub(crate) fn drm_prop_crtc_mode_id(dev: *mut DrmDevice) -> u32 {
    // SAFETY: caller's device.
    unsafe {
        prop_get_or_create(
            dev,
            b"MODE_ID",
            DRM_MODE_PROP_BLOB | DRM_MODE_PROP_ATOMIC,
            DRM_MODE_OBJECT_CRTC,
            &[],
            4,
        )
    }
}

/// `CRTC_ID` on a connector: the CRTC it is bound to (0 detaches).
pub(crate) fn drm_prop_connector_crtc_id(dev: *mut DrmDevice) -> u32 {
    // SAFETY: caller's device.
    unsafe {
        prop_get_or_create(
            dev,
            b"CRTC_ID",
            DRM_MODE_PROP_OBJECT | DRM_MODE_PROP_ATOMIC,
            DRM_MODE_OBJECT_CONNECTOR,
            &[],
            4,
        )
    }
}

/// `CRTC_ID` on a plane: the CRTC the plane is attached to (0 detaches).
pub(crate) fn drm_prop_plane_crtc_id(dev: *mut DrmDevice) -> u32 {
    // SAFETY: caller's device.
    unsafe {
        prop_get_or_create(
            dev,
            b"CRTC_ID",
            DRM_MODE_PROP_OBJECT | DRM_MODE_PROP_ATOMIC,
            DRM_MODE_OBJECT_PLANE,
            &[],
            4,
        )
    }
}

/// `FB_ID` on a plane: the framebuffer it scans out (0 turns it off).
pub(crate) fn drm_prop_plane_fb_id(dev: *mut DrmDevice) -> u32 {
    // SAFETY: caller's device.
    unsafe {
        prop_get_or_create(
            dev,
            b"FB_ID",
            DRM_MODE_PROP_OBJECT | DRM_MODE_PROP_ATOMIC,
            DRM_MODE_OBJECT_PLANE,
            &[],
            4,
        )
    }
}

/// The plane's geometry properties, in the order
/// `SRC_X, SRC_Y, SRC_W, SRC_H, CRTC_X, CRTC_Y, CRTC_W, CRTC_H`.
/// The source rectangle is 16.16 fixed point, so its range is the u32 one.
pub(crate) fn drm_prop_plane_rects(dev: *mut DrmDevice) -> [u32; 8] {
    const U32_RANGE: [u64; 2] = [0, 0xFFFF_FFFF];
    const I32_RANGE: [u64; 2] = [0x8000_0000, 0x7FFF_FFFF];
    let names: [&[u8]; 8] = [
        b"SRC_X", b"SRC_Y", b"SRC_W", b"SRC_H", b"CRTC_X", b"CRTC_Y", b"CRTC_W", b"CRTC_H",
    ];
    let mut ids = [0u32; 8];
    for (i, name) in names.iter().enumerate() {
        // SAFETY: caller's device.
        ids[i] = unsafe {
            let signed = i == 4 || i == 5; // CRTC_X / CRTC_Y may be negative
            prop_get_or_create(
                dev,
                name,
                if signed {
                    DRM_MODE_PROP_SIGNED_RANGE | DRM_MODE_PROP_ATOMIC
                } else {
                    DRM_MODE_PROP_RANGE | DRM_MODE_PROP_ATOMIC
                },
                DRM_MODE_OBJECT_PLANE,
                if signed { &I32_RANGE } else { &U32_RANGE },
                4,
            )
        };
    }
    ids
}

/// The name of a property, NUL-padded, and its object type — what the atomic
/// handler needs to interpret a `(prop_id, value)` pair.
pub(crate) fn drm_prop_name_and_type(dev: *mut DrmDevice, prop_id: u32) -> ([u8; PROPERTY_NAME_LEN], u32) {
    let mut name = [0u8; PROPERTY_NAME_LEN];
    if dev.is_null() {
        return (name, 0);
    }
    // SAFETY: caller's device; read-only.
    unsafe {
        let p = (*dev).find_prop(prop_id);
        if p.is_null() {
            return (name, 0);
        }
        name = (*p).name;
        (name, (*p).ptype)
    }
}
