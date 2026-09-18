//! Framebuffer objects and their ioctls: ADDFB, ADDFB2, RMFB, GETFB2, DIRTYFB.
//!
//! A framebuffer pins the GEM object it scans out; a CRTC keeps its own
//! reference for as long as it is on screen, so removing a framebuffer (RMFB)
//! drops the client's reference but leaves a scanning-out CRTC's alone.
//!
//! The device's framebuffer list and the client's list of framebuffers it
//! created are both allocator-backed, so neither is capped.

use core::ffi::{c_int, c_void};

use super::mode_object::Crtc;
use crate::device::drm_driver_dirty;
use crate::ffi::{drm_copy_in, drm_copy_out};
use crate::structs::{DrmDevice, DrmFile, GemObject, IrqSpinlock};

/* ── the kernel-side object (drm_drv.h) ─────────────────────────────────── */

/// `struct drm_framebuffer` — 40 bytes.
#[repr(C)]
pub struct Framebuffer {
    pub dev: *mut DrmDevice,
    pub id: u32,
    pub width: u32,
    pub height: u32,
    pub pitch: u32,
    pub format: u32,
    pub flags: u32,
    pub modifier: u32,
    pub obj: *mut GemObject,
    pub offset: u32,
}

const _: () = assert!(core::mem::size_of::<Framebuffer>() == 40);
const _: () = assert!(core::mem::offset_of!(Framebuffer, width) == 8);
const _: () = assert!(core::mem::offset_of!(Framebuffer, format) == 20);
const _: () = assert!(core::mem::offset_of!(Framebuffer, obj) == 32);
const _: () = assert!(core::mem::offset_of!(Framebuffer, offset) == 36);

/* ── uapi structs ───────────────────────────────────────────────────────── */

/// `struct drm_mode_fb_cmd` (ADDFB v1) — 28 bytes.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct ModeFbCmd {
    pub fb_id: u32,
    pub width: u32,
    pub height: u32,
    pub pitch: u32,
    pub bpp: u32,
    pub depth: u32,
    pub handle: u32,
}

/// `struct drm_mode_fb_cmd2` (ADDFB2 / GETFB2) — 100 bytes.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct ModeFbCmd2 {
    pub fb_id: u32,
    pub width: u32,
    pub height: u32,
    pub pixel_format: u32,
    pub flags: u32,
    pub handles: [u32; 4],
    pub pitches: [u32; 4],
    pub offsets: [u32; 4],
    pub modifier: [u64; 4],
}

/// `struct drm_mode_fb_dirty_cmd` — 24 bytes.
#[repr(C)]
pub struct ModeFbDirtyCmd {
    pub fb_id: u32,
    pub flags: u32,
    pub color: u32,
    pub num_clips: u32,
    pub clips_ptr: u64,
}

/// `struct drm_clip_rect` — 8 bytes.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct ClipRect {
    pub x1: u16,
    pub y1: u16,
    pub x2: u16,
    pub y2: u16,
}

const _: () = assert!(core::mem::size_of::<ModeFbCmd>() == 28);
const _: () = assert!(core::mem::offset_of!(ModeFbCmd, pitch) == 12);
const _: () = assert!(core::mem::offset_of!(ModeFbCmd, depth) == 20);
const _: () = assert!(core::mem::offset_of!(ModeFbCmd, handle) == 24);

const _: () = assert!(core::mem::size_of::<ModeFbCmd2>() == 100);
const _: () = assert!(core::mem::offset_of!(ModeFbCmd2, pixel_format) == 12);
const _: () = assert!(core::mem::offset_of!(ModeFbCmd2, handles) == 20);
const _: () = assert!(core::mem::offset_of!(ModeFbCmd2, pitches) == 36);
const _: () = assert!(core::mem::offset_of!(ModeFbCmd2, offsets) == 52);
const _: () = assert!(core::mem::offset_of!(ModeFbCmd2, modifier) == 68);

const _: () = assert!(core::mem::size_of::<ModeFbDirtyCmd>() == 24);
const _: () = assert!(core::mem::size_of::<ClipRect>() == 8);

/* ioctl numbers (uapi/drm.h): DRM_IOWR of the listed size. */
const fn drm_iowr(nr: u32, size: u32) -> u32 {
    (3u32 << 30) | (0x64u32 << 8) | nr | (size << 16)
}
const DRM_IOCTL_MODE_ADDFB: u32 = drm_iowr(0xAE, 28);
const DRM_IOCTL_MODE_RMFB: u32 = drm_iowr(0xAF, 4);
const DRM_IOCTL_MODE_DIRTYFB: u32 = drm_iowr(0xB1, 24);
const DRM_IOCTL_MODE_ADDFB2: u32 = drm_iowr(0xB8, 100);
const DRM_IOCTL_MODE_GETFB2: u32 = drm_iowr(0xCE, 100);

/* The few DRM_FORMAT_* fourccs ADDFB v1 needs (uapi/drm_fourcc.h). */
const fn fourcc(a: u8, b: u8, c: u8, d: u8) -> u32 {
    (a as u32) | ((b as u32) << 8) | ((c as u32) << 16) | ((d as u32) << 24)
}
const DRM_FORMAT_RGB565: u32 = fourcc(b'R', b'G', b'1', b'6');
const DRM_FORMAT_RGB888: u32 = fourcc(b'R', b'G', b'2', b'4');
const DRM_FORMAT_XRGB8888: u32 = fourcc(b'X', b'R', b'2', b'4');

/* uapi/drm_fourcc.h: the only layout this device's 2D resources have. */
const DRM_FORMAT_MOD_LINEAR: u32 = 0;

/* One DIRTYFB request copies at most this many clip rectangles in one go; the
 * client is free to split a larger dirty region. */
const MAX_DIRTY_CLIPS: usize = 16;

extern "C" {
    fn kmalloc(size: u32) -> *mut c_void;
    fn kfree(p: *mut c_void);
    fn irq_spinlock_acquire(lock: *mut IrqSpinlock);
    fn irq_spinlock_release(lock: *mut IrqSpinlock);
    fn validate_user_ptr(ptr: *const c_void, size: u32) -> c_int;
    fn copy_from_user(dst: *mut c_void, src: *const c_void, size: u32) -> c_int;

    fn drm_gem_handle_lookup(file: *mut DrmFile, handle: u32) -> *mut GemObject;
    fn drm_gem_handle_create(
        file: *mut DrmFile,
        obj: *mut GemObject,
        handle_out: *mut u32,
    ) -> c_int;
    fn drm_gem_ref(obj: *mut GemObject);
    fn drm_gem_unref(obj: *mut GemObject);
}

#[inline]
unsafe fn lock_ptr(dev: *mut DrmDevice) -> *mut IrqSpinlock {
    core::ptr::addr_of_mut!((*dev).lock)
}

/* ── lookup ─────────────────────────────────────────────────────────────── */

/// `drm_fb_find` — look a framebuffer up by id.
#[no_mangle]
pub extern "C" fn drm_fb_find(dev: *mut DrmDevice, id: u32) -> *mut c_void {
    if dev.is_null() {
        return core::ptr::null_mut();
    }
    // SAFETY: read-only walk of the caller's live device.
    unsafe { (*dev).find_fb(id) }
}

/* ── lifetime ───────────────────────────────────────────────────────────── */

/// `drm_fb_handle_init` — create a framebuffer and register it for `file`.
#[no_mangle]
#[allow(clippy::too_many_arguments)]
pub extern "C" fn drm_fb_handle_init(
    file: *mut DrmFile,
    width: u32,
    height: u32,
    pitch: u32,
    format: u32,
    flags: u32,
    modifier: u32,
    _depth: c_int,
    _bpp: c_int,
    obj: *mut GemObject,
    offset: u32,
    fb_id_out: *mut u32,
) -> c_int {
    if file.is_null() || obj.is_null() || fb_id_out.is_null() {
        return -22;
    }
    // SAFETY: caller's file and object.
    let dev = unsafe { (*file).dev };
    if dev.is_null() {
        return -22;
    }

    if crate::mode::drm_format_bpp(format) == 0 {
        return -22;
    }
    /* virtio-gpu's 2D resources are linear only, so DRM_FORMAT_MOD_LINEAR is
     * the one modifier accepted; anything else is refused rather than silently
     * treated as linear. */
    if modifier != DRM_FORMAT_MOD_LINEAR {
        return -22;
    }
    if width == 0 || height == 0 || pitch == 0 {
        return -22;
    }
    if height > 0xFFFF_FFFFu32 / pitch {
        return -22;
    }
    // The framebuffer has to fit inside the object it points at.
    let need = offset as u64 + pitch as u64 * height as u64;
    if need > unsafe { (*obj).size } as u64 {
        return -22;
    }

    // SAFETY: fresh allocation, then filled field by field.
    unsafe {
        let mem = kmalloc(core::mem::size_of::<Framebuffer>() as u32) as *mut Framebuffer;
        if mem.is_null() {
            return -12;
        }
        core::ptr::write_bytes(mem, 0, 1);
        let fb = &mut *mem;
        fb.dev = dev;
        fb.width = width;
        fb.height = height;
        fb.pitch = pitch;
        fb.format = format;
        fb.flags = flags;
        fb.modifier = modifier;
        fb.obj = obj;
        fb.offset = offset;
        drm_gem_ref(obj);

        irq_spinlock_acquire(lock_ptr(dev));
        /* Ids only move forward, so a removed framebuffer's id is never handed
         * out again while the device lives. */
        (*dev).next_fb_id += 1;
        fb.id = (*dev).next_fb_id;
        (*dev).fbs.push(mem as *mut c_void);
        irq_spinlock_release(lock_ptr(dev));

        (*file).fbs.push(fb.id);
        *fb_id_out = fb.id;
    }
    0
}

/// `drm_fb_handle_release` — drop the client's reference to a framebuffer.
#[no_mangle]
pub extern "C" fn drm_fb_handle_release(file: *mut DrmFile, fb_id: u32) {
    if file.is_null() {
        return;
    }
    // SAFETY: caller's file/device; the pools are walked under the device lock.
    unsafe {
        let dev = (*file).dev;
        if dev.is_null() {
            return;
        }

        match (*file).fbs.iter().position(|&x| x == fb_id) {
            Some(pos) => {
                (*file).fbs.remove(pos);
            }
            None => return,
        }

        let fb = drm_fb_find(dev, fb_id) as *mut Framebuffer;
        if fb.is_null() {
            return;
        }

        /* A CRTC scanning this out keeps its own reference through crtc->fb,
         * so it only forgets the pointer here. */
        for &c in (*dev).crtcs.iter() {
            let crtc = c as *mut Crtc;
            if !crtc.is_null() && (*crtc).fb == fb as *mut c_void {
                (*crtc).fb = core::ptr::null_mut();
            }
        }

        irq_spinlock_acquire(lock_ptr(dev));
        if let Some(pos) = (*dev).fbs.iter().position(|&p| p == fb as *mut c_void) {
            (*dev).fbs.remove(pos);
        }
        irq_spinlock_release(lock_ptr(dev));

        if !(*fb).obj.is_null() {
            drm_gem_unref((*fb).obj);
        }
        kfree(fb as *mut c_void);
    }
}

/* ── ioctls ─────────────────────────────────────────────────────────────── */

/// `DRM_IOCTL_MODE_ADDFB2`.
#[no_mangle]
pub extern "C" fn mode_addfb2(file: *mut DrmFile, arg: *mut c_void) -> c_int {
    if file.is_null() {
        return -22;
    }
    let mut req = ModeFbCmd2 {
        fb_id: 0,
        width: 0,
        height: 0,
        pixel_format: 0,
        flags: 0,
        handles: [0; 4],
        pitches: [0; 4],
        offsets: [0; 4],
        modifier: [0; 4],
    };
    if drm_copy_in(
        &mut req as *mut _ as *mut c_void,
        DRM_IOCTL_MODE_ADDFB2,
        arg,
        core::mem::size_of::<ModeFbCmd2>() as u32,
    ) != 0
    {
        return -22;
    }

    // SAFETY: caller's file.
    let obj = unsafe { drm_gem_handle_lookup(file, req.handles[0]) };
    if obj.is_null() {
        return -22;
    }
    /* Only single-plane formats are supported: nothing here scans out a YUV
     * plane set, and claiming otherwise would mislead userspace. */
    if req.handles[1] != 0 || req.handles[2] != 0 || req.handles[3] != 0 {
        return -22;
    }

    let mut fb_id: u32 = 0;
    let rc = drm_fb_handle_init(
        file,
        req.width,
        req.height,
        req.pitches[0],
        req.pixel_format,
        req.flags,
        req.modifier[0] as u32,
        0,
        0,
        obj,
        req.offsets[0],
        &mut fb_id,
    );
    if rc != 0 {
        return rc;
    }

    req.fb_id = fb_id;
    drm_copy_out(arg, &req as *const _ as *const c_void, core::mem::size_of::<ModeFbCmd2>() as u32)
}

/// `DRM_IOCTL_MODE_ADDFB` (v1: depth/bpp instead of a fourcc).
#[no_mangle]
pub extern "C" fn mode_addfb(file: *mut DrmFile, arg: *mut c_void) -> c_int {
    if file.is_null() {
        return -22;
    }
    let mut req = ModeFbCmd {
        fb_id: 0,
        width: 0,
        height: 0,
        pitch: 0,
        bpp: 0,
        depth: 0,
        handle: 0,
    };
    if drm_copy_in(
        &mut req as *mut _ as *mut c_void,
        DRM_IOCTL_MODE_ADDFB,
        arg,
        core::mem::size_of::<ModeFbCmd>() as u32,
    ) != 0
    {
        return -22;
    }

    // SAFETY: caller's file.
    let obj = unsafe { drm_gem_handle_lookup(file, req.handle) };
    if obj.is_null() {
        return -22;
    }

    let fourcc = match req.bpp {
        16 => DRM_FORMAT_RGB565,
        24 => DRM_FORMAT_RGB888,
        32 => DRM_FORMAT_XRGB8888,
        _ => return -22,
    };

    let mut fb_id: u32 = 0;
    let rc = drm_fb_handle_init(
        file,
        req.width,
        req.height,
        req.pitch,
        fourcc,
        0,
        0,
        req.depth as c_int,
        req.bpp as c_int,
        obj,
        0,
        &mut fb_id,
    );
    if rc != 0 {
        return rc;
    }

    req.fb_id = fb_id;
    drm_copy_out(arg, &req as *const _ as *const c_void, core::mem::size_of::<ModeFbCmd>() as u32)
}

/// `DRM_IOCTL_MODE_RMFB` — the payload is a bare `unsigned int` fb id.
#[no_mangle]
pub extern "C" fn mode_rmfb(file: *mut DrmFile, arg: *mut c_void) -> c_int {
    if file.is_null() {
        return -22;
    }
    let mut fb_id: u32 = 0;
    if drm_copy_in(
        &mut fb_id as *mut _ as *mut c_void,
        DRM_IOCTL_MODE_RMFB,
        arg,
        4,
    ) != 0
    {
        return -22;
    }
    drm_fb_handle_release(file, fb_id);
    0
}

/// `DRM_IOCTL_MODE_GETFB2`.
#[no_mangle]
pub extern "C" fn mode_getfb2(file: *mut DrmFile, arg: *mut c_void) -> c_int {
    if file.is_null() {
        return -22;
    }
    let mut req = ModeFbCmd2 {
        fb_id: 0,
        width: 0,
        height: 0,
        pixel_format: 0,
        flags: 0,
        handles: [0; 4],
        pitches: [0; 4],
        offsets: [0; 4],
        modifier: [0; 4],
    };
    if drm_copy_in(
        &mut req as *mut _ as *mut c_void,
        DRM_IOCTL_MODE_GETFB2,
        arg,
        core::mem::size_of::<ModeFbCmd2>() as u32,
    ) != 0
    {
        return -22;
    }

    // SAFETY: caller's file.
    let dev = unsafe { (*file).dev };
    let fb = drm_fb_find(dev, req.fb_id) as *mut Framebuffer;
    if fb.is_null() {
        return -22;
    }

    // SAFETY: looked up in the caller's device.
    unsafe {
        let mut handle: u32 = 0;
        if drm_gem_handle_create(file, (*fb).obj, &mut handle) != 0 {
            return -12;
        }
        req.fb_id = (*fb).id;
        req.width = (*fb).width;
        req.height = (*fb).height;
        req.pixel_format = (*fb).format;
        req.flags = (*fb).flags;
        req.handles[0] = handle;
        req.pitches[0] = (*fb).pitch;
        req.offsets[0] = (*fb).offset;
        req.modifier[0] = (*fb).modifier as u64;
    }
    drm_copy_out(arg, &req as *const _ as *const c_void, core::mem::size_of::<ModeFbCmd2>() as u32)
}

/// `DRM_IOCTL_MODE_DIRTYFB`.
#[no_mangle]
pub extern "C" fn mode_dirtyfb(file: *mut DrmFile, arg: *mut c_void) -> c_int {
    if file.is_null() {
        return -22;
    }
    let mut req = ModeFbDirtyCmd {
        fb_id: 0,
        flags: 0,
        color: 0,
        num_clips: 0,
        clips_ptr: 0,
    };
    if drm_copy_in(
        &mut req as *mut _ as *mut c_void,
        DRM_IOCTL_MODE_DIRTYFB,
        arg,
        core::mem::size_of::<ModeFbDirtyCmd>() as u32,
    ) != 0
    {
        return -22;
    }

    // SAFETY: caller's file.
    let dev = unsafe { (*file).dev };
    let fb = drm_fb_find(dev, req.fb_id) as *mut Framebuffer;
    if fb.is_null() {
        return -22;
    }

    /* Clip rectangles come from userspace; keep the copy small and bounded —
     * this is the kernel stack. */
    let mut clips = [ClipRect {
        x1: 0,
        y1: 0,
        x2: 0,
        y2: 0,
    }; MAX_DIRTY_CLIPS];
    let n = if req.num_clips as usize > MAX_DIRTY_CLIPS {
        MAX_DIRTY_CLIPS
    } else {
        req.num_clips as usize
    };
    if n != 0 && req.clips_ptr != 0 {
        let user = req.clips_ptr as u32 as usize as *mut c_void;
        let bytes = (n * core::mem::size_of::<ClipRect>()) as u32;
        // SAFETY: `user` is the client's pointer; this is the same kernel range
        // check plus copy pair the C version used.
        unsafe {
            if validate_user_ptr(user, bytes) == 0 {
                return -22;
            }
            if copy_from_user(clips.as_mut_ptr() as *mut c_void, user, bytes) != 0 {
                return -22;
            }
        }
    }

    drm_driver_dirty(dev, fb, clips.as_ptr(), n as u32)
}
