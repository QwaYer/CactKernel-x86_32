//! The ioctl entry point: the dispatcher that splits the legacy range from the
//! KMS range and from driver-private commands, plus the legacy (pre-KMS) ioctls
//! themselves.
//!
//! Migrated slice 12 — `core/drm_ioctl.c`.  The payload copy helpers
//! (`drm_ioctl_nr`, `drm_ioctl_size`, `drm_copy_in`, `drm_copy_out`) live in
//! `crate::ffi` and their declarations do not change.

use core::ffi::{c_int, c_void};

use crate::event::{drm_crtc_vblank_advance, drm_file_queue_event};
use crate::ffi::{drm_copy_in, drm_copy_out, drm_ioctl_nr, drm_ioctl_size};
use crate::gem::{
    drm_gem_handle_close, drm_gem_handle_create, drm_gem_handle_lookup, drm_gem_prime_fd_to_handle,
    drm_gem_prime_handle_to_fd, drm_gem_ref,
};
use crate::kms::mode_ioctl::drm_mode_ioctl;
use crate::kms::mode_object::{drm_crtc_find, Crtc};
use crate::structs::{DrmDevice, DrmFile, GemObject};

/* Longest ioctl payload the core decodes on a kernel stack copy. */
const DRM_IOCTL_MAX_SIZE: u32 = 256;

/* uapi/drm.h — the command band boundaries and the two vblank flags used. */
const DRM_COMMAND_BASE: u32 = 0x40;
const DRM_COMMAND_END: u32 = 0xA0;
const DRM_EVENT_VBLANK: u32 = 0x01;
const _DRM_VBLANK_RELATIVE: u32 = 0x1;
const _DRM_VBLANK_EVENT: u32 = 0x400_0000;
const _DRM_VBLANK_HIGH_CRTC_MASK: u32 = 0x0000_003e;
const _DRM_VBLANK_HIGH_CRTC_SHIFT: u32 = 1;

/* How long a blocking WAIT_VBLANK will spin before giving up and reporting the
 * sequence it has (100 Hz ticks, so 30 ticks is about 300 ms).  The counter
 * only moves with time, so a client asking for a far-future sequence must not
 * be able to pin the CPU. */
const VBLANK_WAIT_TICKS: u32 = 30;

extern "C" {
    /// Scheduler sleep, in 100 Hz ticks (`proc.h`); also used by rust_net.
    fn sched_sleep_ticks(ticks: u32);
}

/* ── uapi structs ───────────────────────────────────────────────────────── */

#[repr(C)]
struct DrmVersion {
    version_major: c_int,
    version_minor: c_int,
    version_patchlevel: c_int,
    name_len: u32, // __kernel_size_t on i386
    name: *mut u8,
    date_len: u32,
    date: *mut u8,
    desc_len: u32,
    desc: *mut u8,
}

#[repr(C)]
struct DrmGetCap {
    capability: u64,
    value: u64,
}

#[repr(C)]
struct DrmSetClientCap {
    capability: u64,
    value: u64,
}

#[repr(C)]
struct DrmAuth {
    magic: u32,
}

#[repr(C)]
struct DrmGemClose {
    handle: u32,
    pad: u32,
}

#[repr(C)]
struct DrmGemFlink {
    handle: u32,
    name: u32,
}

#[repr(C)]
struct DrmGemOpen {
    name: u32,
    handle: u32,
    size: u64,
}

#[repr(C)]
struct DrmPrimeHandle {
    handle: u32,
    flags: u32,
    fd: c_int,
}

/// `union drm_wait_vblank` flattened: `request` (type, sequence, signal) and
/// `reply` (type, sequence, tval_sec, tval_usec) overlap, so both views share
/// the same four slots, and the whole 16 bytes are what the ioctl copies.
#[repr(C)]
#[derive(Clone, Copy)]
struct WaitVblank {
    vtype: u32,     // request.type / reply.type
    sequence: u32,  // request.sequence / reply.sequence
    signal: u32,    // request.signal / reply.tval_sec
    tval_usec: i32, // reply.tval_usec
}

const _: () = assert!(core::mem::size_of::<DrmVersion>() == 36);
const _: () = assert!(core::mem::offset_of!(DrmVersion, name) == 16);
const _: () = assert!(core::mem::size_of::<DrmGetCap>() == 16);
const _: () = assert!(core::mem::size_of::<DrmSetClientCap>() == 16);
const _: () = assert!(core::mem::size_of::<DrmAuth>() == 4);
const _: () = assert!(core::mem::size_of::<DrmGemClose>() == 8);
const _: () = assert!(core::mem::size_of::<DrmGemFlink>() == 8);
const _: () = assert!(core::mem::size_of::<DrmGemOpen>() == 16);
const _: () = assert!(core::mem::size_of::<DrmPrimeHandle>() == 12);
const _: () = assert!(core::mem::size_of::<WaitVblank>() == 16);

/* ioctl numbers: `_IOC(dir, 'd', nr, size)`. */
const fn drm_ioc(dir: u32, nr: u32, size: u32) -> u32 {
    (dir << 30) | (0x64u32 << 8) | nr | (size << 16)
}
const DRM_IOCTL_VERSION: u32 = drm_ioc(3, 0x00, 36);
const DRM_IOCTL_GET_MAGIC: u32 = drm_ioc(2, 0x02, 4);
const DRM_IOCTL_GEM_CLOSE: u32 = drm_ioc(1, 0x09, 8);
const DRM_IOCTL_GEM_FLINK: u32 = drm_ioc(3, 0x0a, 8);
const DRM_IOCTL_GEM_OPEN: u32 = drm_ioc(3, 0x0b, 16);
const DRM_IOCTL_GET_CAP: u32 = drm_ioc(3, 0x0c, 16);
const DRM_IOCTL_SET_CLIENT_CAP: u32 = drm_ioc(1, 0x0d, 16);
const DRM_IOCTL_AUTH_MAGIC: u32 = drm_ioc(1, 0x11, 4);
const DRM_IOCTL_PRIME_HANDLE_TO_FD: u32 = drm_ioc(3, 0x2d, 12);
const DRM_IOCTL_PRIME_FD_TO_HANDLE: u32 = drm_ioc(3, 0x2e, 12);
const DRM_IOCTL_WAIT_VBLANK: u32 = drm_ioc(3, 0x3a, 16);

/* Sequence numbers (== _IOC_NR of the numbers above). */
const NR_VERSION: u32 = 0x00;
const NR_GET_MAGIC: u32 = 0x02;
const NR_GEM_CLOSE: u32 = 0x09;
const NR_GEM_FLINK: u32 = 0x0a;
const NR_GEM_OPEN: u32 = 0x0b;
const NR_GET_CAP: u32 = 0x0c;
const NR_SET_CLIENT_CAP: u32 = 0x0d;
const NR_AUTH_MAGIC: u32 = 0x11;
const NR_SET_MASTER: u32 = 0x1e;
const NR_DROP_MASTER: u32 = 0x1f;
const NR_PRIME_HANDLE_TO_FD: u32 = 0x2d;
const NR_PRIME_FD_TO_HANDLE: u32 = 0x2e;
const NR_WAIT_VBLANK: u32 = 0x3a;

/* DRM_CLIENT_CAP_* / DRM_CAP_* (uapi/drm.h). */
const DRM_CLIENT_CAP_UNIVERSAL_PLANES: u64 = 2;
const DRM_CLIENT_CAP_ATOMIC: u64 = 3;

/// `strlen` on a C string.
unsafe fn cstrlen(p: *const u8) -> u32 {
    if p.is_null() {
        return 0;
    }
    let mut n = 0u32;
    loop {
        // SAFETY: `p` is non-null (checked above) and the caller guarantees it points
        // at a NUL-terminated string, so this offset is a readable byte.
        let q = unsafe { p.add(n as usize) };
        // SAFETY: `q` points at one byte of that string.
        if unsafe { *q } == 0 {
            break;
        }
        n += 1;
    }
    n
}

/* ── the dispatcher ─────────────────────────────────────────────────────── */

#[no_mangle]
pub extern "C" fn drm_ioctl_dispatch(file: *mut DrmFile, cmd: u32, arg: *mut c_void) -> c_int {
    let nr = drm_ioctl_nr(cmd);
    let size = drm_ioctl_size(cmd);

    if size > DRM_IOCTL_MAX_SIZE {
        // SAFETY: fixed format string, no arguments.
        unsafe {
            crate::ffi::printk(
                c"\x017  drm         : ioctl payload too large\n".as_ptr() as *const u8,
            );
        }
        return -22;
    }

    /* Core ranges first, then whatever the driver claimed.  The DRM numbering
     * is three bands, not two:
     *   nr <  DRM_COMMAND_BASE  legacy/core commands (VERSION, GEM_*, PRIME…)
     *   DRM_COMMAND_BASE .. DRM_COMMAND_END - 1
     *                           driver-private commands (DRM_IOCTL_VIRTGPU_*)
     *   nr >= DRM_COMMAND_END   KMS again (0xA0 GETRESOURCES … 0xD0), which is
     *                           core-owned just like the legacy band.
     * Getting this boundary wrong sends GETRESOURCES/SETCRTC to the driver,
     * which then has nothing to say about them. */
    if !(DRM_COMMAND_BASE..DRM_COMMAND_END).contains(&nr) {
        let rc = drm_legacy_ioctl(file, nr, arg);
        if rc != -38 {
            return rc; // -ENOSYS means "not mine"
        }
        let rc = drm_mode_ioctl(file, nr, arg);
        if rc != -38 {
            return rc;
        }
        return -22;
    }

    if file.is_null() {
        return -22;
    }
    // SAFETY: `file` is the caller's live client (checked non-null above), so this
    // `dev` field read is in bounds.
    let dev = unsafe { (*file).dev };
    if dev.is_null() {
        return -22;
    }
    // SAFETY: `dev` is the client's live device, so this ops-table read is in bounds.
    let ops = unsafe { (*dev).ops };
    if !ops.is_null() {
        // SAFETY: `ops` is the driver's live ops table; this only copies the ioctl
        // function pointer.
        let dispatch = unsafe { (*ops).ioctl };
        if let Some(dispatch) = dispatch {
            return dispatch(dev, file, cmd, arg, size);
        }
    }
    -22
}

/* ── legacy ioctls ──────────────────────────────────────────────────────── */

/* Global GEM names (GEM_FLINK / GEM_OPEN).  Kept per device, one map entry per
 * named object, so there is no cap and a name is never reused. */
unsafe fn drm_flink_name(dev: *mut DrmDevice, obj: *mut GemObject, name_out: *mut u32) -> c_int {
    // SAFETY: `dev` is the caller's live device; the flink map is only touched here,
    // under the caller's device lock, so this borrow is exclusive for the update.
    let dev = unsafe { &mut *dev };
    for (&name, &o) in dev.flink.iter() {
        if o == obj {
            // SAFETY: `name_out` points at a caller-owned `u32` to receive the name.
            unsafe { *name_out = name };
            return 0;
        }
    }
    dev.next_flink_name += 1;
    let name = dev.next_flink_name;
    dev.flink.insert(name, obj);
    drm_gem_ref(obj);
    // SAFETY: as above — writing the freshly-assigned name to the caller's slot.
    unsafe { *name_out = name };
    0
}

unsafe fn drm_flink_lookup(dev: *mut DrmDevice, name: u32) -> *mut GemObject {
    // SAFETY: `dev` is the caller's live device; this only reads its flink map.
    unsafe {
        match (*dev).flink.get(&name) {
            Some(&o) => o,
            None => core::ptr::null_mut(),
        }
    }
}

#[no_mangle]
pub extern "C" fn drm_legacy_ioctl(file: *mut DrmFile, nr: u32, arg: *mut c_void) -> c_int {
    if file.is_null() {
        return -22;
    }
    // SAFETY: `file` is the caller's live client (checked non-null at entry), so
    // this `dev` field read is in bounds.
    let dev = unsafe { (*file).dev };
    // SAFETY: `dev` is the client's live device, so this ops-table read is in bounds.
    let ops = unsafe { (*dev).ops };

    {
        match nr {
            NR_VERSION => {
                let mut v = DrmVersion {
                    version_major: 0,
                    version_minor: 0,
                    version_patchlevel: 0,
                    name_len: 0,
                    name: core::ptr::null_mut(),
                    date_len: 0,
                    date: core::ptr::null_mut(),
                    desc_len: 0,
                    desc: core::ptr::null_mut(),
                };
                if drm_copy_in(
                    &mut v as *mut _ as *mut c_void,
                    DRM_IOCTL_VERSION,
                    arg,
                    core::mem::size_of::<DrmVersion>() as u32,
                ) != 0
                {
                    return -22;
                }

                let ops_name = if ops.is_null() {
                    core::ptr::null()
                } else {
                    // SAFETY: `ops` is the driver's live ops table (null was handled
                    // above); this reads its `name` field.
                    unsafe { (*ops).name }
                };
                let name = if !ops_name.is_null() {
                    ops_name
                } else {
                    c"cact".as_ptr() as *const u8
                };

                /* Userspace passes the buffer it has; copy the name out and
                 * report the full length so the caller can resize and retry,
                 * as libdrm does. */
                // SAFETY: `name` is a NUL-terminated string (a driver static or the
                // literal below), so `cstrlen`'s contract is met.
                let n = unsafe { cstrlen(name) };
                if !v.name.is_null() && v.name_len != 0 {
                    let c = if n < v.name_len { n } else { v.name_len };
                    if drm_copy_out(v.name as *mut c_void, name as *const c_void, c) != 0 {
                        return -22;
                    }
                }
                v.name_len = n;
                v.date = core::ptr::null_mut();
                v.date_len = 0;
                v.desc = core::ptr::null_mut();
                v.desc_len = 0;
                if !ops.is_null() {
                    // SAFETY: `ops` is the driver's live ops table; this reads its
                    // major version.
                    v.version_major = unsafe { (*ops).major } as c_int;
                    // SAFETY: as above — the minor version.
                    v.version_minor = unsafe { (*ops).minor } as c_int;
                    // SAFETY: as above — the patchlevel.
                    v.version_patchlevel = unsafe { (*ops).patchlevel } as c_int;
                }
                if drm_copy_out(
                    arg,
                    &v as *const _ as *const c_void,
                    core::mem::size_of::<DrmVersion>() as u32,
                ) != 0
                {
                    return -22;
                }
                0
            }

            NR_GET_CAP => {
                let mut c = DrmGetCap { capability: 0, value: 0 };
                if drm_copy_in(
                    &mut c as *mut _ as *mut c_void,
                    DRM_IOCTL_GET_CAP,
                    arg,
                    core::mem::size_of::<DrmGetCap>() as u32,
                ) != 0
                {
                    return -22;
                }
                c.value = match c.capability {
                    0x1 => 1,    // DRM_CAP_DUMB_BUFFER
                    0x2 => 1,    // DRM_CAP_VBLANK_HIGH_CRTC
                    0x3 => 24,   // DRM_CAP_DUMB_PREFERRED_DEPTH
                    0x4 => 0,    // DRM_CAP_DUMB_PREFER_SHADOW
                    0x5 => 1,    // DRM_CAP_PRIME
                    0x8 => 64,   // DRM_CAP_CURSOR_WIDTH
                    0x9 => 64,   // DRM_CAP_CURSOR_HEIGHT
                    0x10 => 1,   // DRM_CAP_ADDFB2_MODIFIERS
                    0x7 => 0,    // DRM_CAP_ASYNC_PAGE_FLIP
                    0x11 => 0,   // DRM_CAP_PAGE_FLIP_TARGET
                    0x12 => 1,   // DRM_CAP_CRTC_IN_VBLANK_EVENT
                    0x13 => 1,   // DRM_CAP_SYNCOBJ
                    0x14 => 1,   // DRM_CAP_SYNCOBJ_TIMELINE
                    _ => 0,      // TIMESTAMP_MONOTONIC, …
                };
                drm_copy_out(arg, &c as *const _ as *const c_void, core::mem::size_of::<DrmGetCap>() as u32)
            }

            NR_SET_CLIENT_CAP => {
                let mut c = DrmSetClientCap { capability: 0, value: 0 };
                if drm_copy_in(
                    &mut c as *mut _ as *mut c_void,
                    DRM_IOCTL_SET_CLIENT_CAP,
                    arg,
                    core::mem::size_of::<DrmSetClientCap>() as u32,
                ) != 0
                {
                    return -22;
                }
                /* Universal planes and atomic are both supported now, so a
                 * client is told it may use them. */
                if c.capability == DRM_CLIENT_CAP_UNIVERSAL_PLANES {
                    return if c.value <= 1 { 0 } else { -22 };
                }
                if c.capability == DRM_CLIENT_CAP_ATOMIC {
                    return if c.value <= 1 { 0 } else { -22 };
                }
                if c.value == 0 {
                    0
                } else {
                    -22
                }
            }

            NR_GET_MAGIC => {
                let mut a = DrmAuth { magic: 0 };
                if drm_copy_in(
                    &mut a as *mut _ as *mut c_void,
                    DRM_IOCTL_GET_MAGIC,
                    arg,
                    core::mem::size_of::<DrmAuth>() as u32,
                ) != 0
                {
                    return -22;
                }
                // SAFETY: `file` is the caller's live client, so this `magic` read is
                // in bounds.
                a.magic = unsafe { (*file).magic };
                drm_copy_out(arg, &a as *const _ as *const c_void, core::mem::size_of::<DrmAuth>() as u32)
            }

            NR_SET_MASTER => {
                /* DRM_IOCTL_SET_MASTER is `_IO` — no payload.  (The C version
                 * copied a `drm_auth` in against a zero-size command, which
                 * made the payload check fail, so it always answered -EINVAL.) */
                // SAFETY: `file` is the caller's live client.
                if unsafe { (*file).is_render } != 0 {
                    return -1;
                }
                // SAFETY: `dev` is the client's live device; this borrow of its client
                // list is consumed by the scan below.
                let clients = unsafe { &(*dev).clients };
                for &o in clients.iter() {
                    if o != file && !o.is_null() {
                        // SAFETY: `o` is a live client (non-null, checked above).
                        if unsafe { (*o).is_master } != 0 {
                            return -1; // already taken
                        }
                    }
                }
                // SAFETY: `file` is the caller's live client.
                unsafe { (*file).is_master = 1 };
                0
            }

            NR_DROP_MASTER => {
                // SAFETY: `file` is the caller's live client.
                unsafe { (*file).is_master = 0 };
                0
            }

            NR_AUTH_MAGIC => {
                let mut a = DrmAuth { magic: 0 };
                if drm_copy_in(
                    &mut a as *mut _ as *mut c_void,
                    DRM_IOCTL_AUTH_MAGIC,
                    arg,
                    core::mem::size_of::<DrmAuth>() as u32,
                ) != 0
                {
                    return -22;
                }
                /* Single-user system: authentication is a formality, every
                 * client of the card node is trusted the moment it opens it. */
                // SAFETY: `file` is the caller's live client.
                unsafe { (*file).authenticated = 1 };
                0
            }

            NR_GEM_CLOSE => {
                let mut c = DrmGemClose { handle: 0, pad: 0 };
                if drm_copy_in(
                    &mut c as *mut _ as *mut c_void,
                    DRM_IOCTL_GEM_CLOSE,
                    arg,
                    core::mem::size_of::<DrmGemClose>() as u32,
                ) != 0
                {
                    return -22;
                }
                drm_gem_handle_close(file, c.handle)
            }

            NR_GEM_FLINK => {
                let mut fl = DrmGemFlink { handle: 0, name: 0 };
                if drm_copy_in(
                    &mut fl as *mut _ as *mut c_void,
                    DRM_IOCTL_GEM_FLINK,
                    arg,
                    core::mem::size_of::<DrmGemFlink>() as u32,
                ) != 0
                {
                    return -22;
                }
                let obj = drm_gem_handle_lookup(file, fl.handle);
                if obj.is_null() {
                    return -9; // -EBADF
                }
                // SAFETY: `drm_flink_name`'s contract: `dev` and `obj` are the caller's
                // live device and object, and `fl.name` is a local to receive the name.
                if unsafe { drm_flink_name(dev, obj, &mut fl.name) } != 0 {
                    return -12;
                }
                drm_copy_out(arg, &fl as *const _ as *const c_void, core::mem::size_of::<DrmGemFlink>() as u32)
            }

            NR_GEM_OPEN => {
                let mut op = DrmGemOpen { name: 0, handle: 0, size: 0 };
                if drm_copy_in(
                    &mut op as *mut _ as *mut c_void,
                    DRM_IOCTL_GEM_OPEN,
                    arg,
                    core::mem::size_of::<DrmGemOpen>() as u32,
                ) != 0
                {
                    return -22;
                }
                // SAFETY: `drm_flink_lookup`'s contract: `dev` is the caller's live
                // device, which it only reads.
                let obj = unsafe { drm_flink_lookup(dev, op.name) };
                if obj.is_null() {
                    return -2; // -ENOENT
                }
                let mut handle = 0u32;
                if drm_gem_handle_create(file, obj, &mut handle) != 0 {
                    return -12;
                }
                op.handle = handle;
                op.size = crate::gem::drm_gem_size(obj) as u64;
                drm_copy_out(arg, &op as *const _ as *const c_void, core::mem::size_of::<DrmGemOpen>() as u32)
            }

            NR_PRIME_HANDLE_TO_FD => {
                let mut p = DrmPrimeHandle {
                    handle: 0,
                    flags: 0,
                    fd: -1,
                };
                if drm_copy_in(
                    &mut p as *mut _ as *mut c_void,
                    DRM_IOCTL_PRIME_HANDLE_TO_FD,
                    arg,
                    core::mem::size_of::<DrmPrimeHandle>() as u32,
                ) != 0
                {
                    return -22;
                }
                let mut fd = -1;
                let rc = drm_gem_prime_handle_to_fd(dev, file, p.handle, p.flags, &mut fd);
                if rc != 0 {
                    return rc;
                }
                p.fd = fd;
                drm_copy_out(arg, &p as *const _ as *const c_void, core::mem::size_of::<DrmPrimeHandle>() as u32)
            }

            NR_PRIME_FD_TO_HANDLE => {
                let mut p = DrmPrimeHandle {
                    handle: 0,
                    flags: 0,
                    fd: -1,
                };
                if drm_copy_in(
                    &mut p as *mut _ as *mut c_void,
                    DRM_IOCTL_PRIME_FD_TO_HANDLE,
                    arg,
                    core::mem::size_of::<DrmPrimeHandle>() as u32,
                ) != 0
                {
                    return -22;
                }
                let rc = drm_gem_prime_fd_to_handle(dev, file, p.fd, &mut p.handle);
                if rc != 0 {
                    return rc;
                }
                drm_copy_out(arg, &p as *const _ as *const c_void, core::mem::size_of::<DrmPrimeHandle>() as u32)
            }

            NR_WAIT_VBLANK => {
                let mut wv = WaitVblank {
                    vtype: 0,
                    sequence: 0,
                    signal: 0,
                    tval_usec: 0,
                };
                if drm_copy_in(
                    &mut wv as *mut _ as *mut c_void,
                    DRM_IOCTL_WAIT_VBLANK,
                    arg,
                    core::mem::size_of::<WaitVblank>() as u32,
                ) != 0
                {
                    return -22;
                }

                /* There is no crtc field in the vblank request: libdrm encodes
                 * the CRTC *index* in the high bits of request.type
                 * (DRM_VBLANK_HIGH_CRTC), so decode it rather than guessing. */
                let crtc_idx = (wv.vtype & _DRM_VBLANK_HIGH_CRTC_MASK) >> _DRM_VBLANK_HIGH_CRTC_SHIFT;
                // SAFETY: `dev` is the client's live device; this borrow of its CRTC
                // table is consumed by the lookups below.
                let crtcs: &[*mut c_void] = unsafe { &(*dev).crtcs };
                let mut crtc_id = 0u32;
                if let Some(&p) = crtcs.get(crtc_idx as usize) {
                    let c = p as *mut Crtc;
                    if !c.is_null() {
                        // SAFETY: `c` is a live, non-null CRTC.
                        crtc_id = unsafe { (*c).id };
                    }
                }
                if crtc_id == 0 {
                    /* "any CRTC": use the first enabled one, or the first there
                     * is. */
                    for &p in crtcs.iter() {
                        let c = p as *mut Crtc;
                        if !c.is_null() {
                            // SAFETY: `c` is a live, non-null CRTC.
                            if unsafe { (*c).enabled } != 0 {
                                // SAFETY: as above — the enabled CRTC's id.
                                crtc_id = unsafe { (*c).id };
                                break;
                            }
                        }
                    }
                    if crtc_id == 0 {
                        if let Some(&p) = crtcs.first() {
                            let c = p as *mut Crtc;
                            if !c.is_null() {
                                // SAFETY: `c` is a live, non-null CRTC.
                                crtc_id = unsafe { (*c).id };
                            }
                        }
                    }
                }
                let crtc = drm_crtc_find(dev, crtc_id) as *mut Crtc;
                if crtc.is_null() {
                    return -22;
                }

                /* The counter advances with the clock (this device has no
                 * vblank interrupt), so bring it up to date first. */
                drm_crtc_vblank_advance(crtc);

                // SAFETY: `crtc` is the live CRTC located above.
                let cur_seq = unsafe { (*crtc).vblank_count };
                if wv.vtype & _DRM_VBLANK_EVENT != 0 {
                    drm_file_queue_event(dev, crtc, DRM_EVENT_VBLANK, wv.signal as u64, cur_seq);
                } else {
                    /* A blocking wait: relative counts from now, absolute names
                     * the sequence to reach. */
                    let relative = wv.vtype & _DRM_VBLANK_RELATIVE != 0;
                    let target = if relative {
                        cur_seq.wrapping_add(wv.sequence)
                    } else {
                        wv.sequence
                    };
                    let mut ticks = 0u32;
                    // SAFETY: `crtc` is the live CRTC located above; the counter is
                    // re-read each iteration.
                    while unsafe { (*crtc).vblank_count } < target && ticks < VBLANK_WAIT_TICKS {
                        // SAFETY: `sched_sleep_ticks` is a kernel service that suspends
                        // the calling task for the requested number of ticks.
                        unsafe { sched_sleep_ticks(1) };
                        ticks += 1;
                        drm_crtc_vblank_advance(crtc);
                    }
                }

                // SAFETY: `crtc` is the live CRTC located above.
                let seq = unsafe { (*crtc).vblank_count };
                // SAFETY: `ktime_get_usec` is a kernel C service reading the monotonic
                // clock.
                let usec = unsafe { ktime_get_usec() } as u32;
                wv.vtype = 0;
                wv.sequence = seq;
                wv.signal = 0;
                wv.tval_usec = usec as i32;
                drm_copy_out(arg, &wv as *const _ as *const c_void, core::mem::size_of::<WaitVblank>() as u32)
            }

            _ => -38, // -ENOSYS: not a legacy ioctl, try the KMS table
        }
    }
}

extern "C" {
    fn ktime_get_usec() -> u64;
}
