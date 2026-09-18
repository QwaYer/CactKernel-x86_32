//! C ABI of the DRM core — the symbols the kernel's C half calls.
//!
//! Migrated slice 1: the ioctl payload plumbing that used to live in
//! `core/drm_ioctl.c` (`drm_ioctl_nr`, `drm_ioctl_size`, `drm_copy_in`,
//! `drm_copy_out`) and `kms/drm_mode_ioctl.c` (`drm_put_raw`).  They are
//! deliberately pure: no device state, only the `_IOC` encoding and the
//! kernel's user-memory primitives, so they are the cheapest thing to move
//! first and are exercised by every single ioctl.
//!
//! C keeps the dispatcher's shape (the three DRM command bands) and the driver
//! ABI; the object model (GEM, KMS objects, framebuffers, properties) follows in
//! later slices.

use core::ffi::{c_int, c_void};

extern "C" {
    /// Kernel user-pointer validation / copy primitives (see syscall/validate.h).
    fn validate_user_ptr(ptr: *const c_void, size: u32) -> c_int;
    fn copy_from_user(dst: *mut c_void, src: *const c_void, size: u32) -> c_int;
    fn copy_to_user(dst: *mut c_void, src: *const c_void, size: u32) -> c_int;

    /// Kernel `printk`, declared variadic exactly as C has it.  The core calls
    /// it with a fixed format string plus arguments where a log line needs
    /// values (device registration reports its object counts).
    pub(crate) fn printk(fmt: *const u8, ...);
}

/// Message printed by the panic handler.
pub(crate) static PANIC_MSG: &[u8] = b"[rust_drm] PANIC\n\0";

/* ── the _IOC encoding (mirrors uapi/drm_ioctl.h) ───────────────────────── */

const IOC_NRBITS: u32 = 8;
const IOC_TYPEBITS: u32 = 8;
const IOC_SIZEBITS: u32 = 14;

const IOC_NRSHIFT: u32 = 0;
const IOC_TYPESHIFT: u32 = IOC_NRSHIFT + IOC_NRBITS;
const IOC_SIZESHIFT: u32 = IOC_TYPESHIFT + IOC_TYPEBITS;

const IOC_NRMASK: u32 = (1 << IOC_NRBITS) - 1;
const IOC_SIZEMASK: u32 = (1 << IOC_SIZEBITS) - 1;

/// Sequence number of a DRM ioctl command.
#[no_mangle]
pub extern "C" fn drm_ioctl_nr(cmd: u32) -> u32 {
    (cmd >> IOC_NRSHIFT) & IOC_NRMASK
}

/// Payload size encoded in a DRM ioctl command.
#[no_mangle]
pub extern "C" fn drm_ioctl_size(cmd: u32) -> u32 {
    (cmd >> IOC_SIZESHIFT) & IOC_SIZEMASK
}

/// Copy `expect` bytes of an ioctl payload in from userspace.
///
/// A payload shorter than the handler needs is rejected: DRM clients always
/// pass the struct their ioctl number was generated from, and accepting a
/// shorter one would leave fields uninitialised.  Returns 0 or -1, like the C
/// version did.
#[no_mangle]
pub extern "C" fn drm_copy_in(
    dst: *mut c_void,
    cmd: u32,
    user: *mut c_void,
    expect: u32,
) -> c_int {
    if user.is_null() || dst.is_null() {
        return -1;
    }
    let size = drm_ioctl_size(cmd);
    if size != expect {
        // SAFETY: static NUL-terminated string.
        unsafe { printk(b"[drm] ioctl payload size mismatch\n\0".as_ptr()) };
        return -1;
    }
    // SAFETY: `user` is a non-null userspace pointer the caller asked us to
    // read; validate_user_ptr is the kernel's own range check for it.
    unsafe {
        if validate_user_ptr(user, size) == 0 {
            return -1;
        }
        if copy_from_user(dst, user, size) != 0 {
            return -1;
        }
    }
    0
}

/// Copy `size` bytes of an ioctl result back to userspace.  Returns 0 or -1.
#[no_mangle]
pub extern "C" fn drm_copy_out(user: *mut c_void, src: *const c_void, size: u32) -> c_int {
    if user.is_null() || src.is_null() {
        return -1;
    }
    // SAFETY: as above; `src` is a kernel buffer of at least `size` bytes.
    unsafe {
        if validate_user_ptr(user, size) == 0 {
            return -1;
        }
        if copy_to_user(user, src, size) != 0 {
            return -1;
        }
    }
    0
}

/// A userspace pointer carried in a uapi struct (`__u64`), i.e. C's `UPTR()`.
///
/// The kernel is 32-bit, so this truncates exactly the way `(void
/// *)(uintptr_t)x` does on that C side.
pub(crate) fn uptru(x: u64) -> *mut c_void {
    x as u32 as usize as *mut c_void
}

/// Copy a kernel buffer to an optional userspace pointer.
///
/// The uapi structs carry userspace addresses as `__u64`, and a client that
/// only wants the counts leaves such a field zero — that is a "no buffer"
/// request, not an error, so a null/zero-size request succeeds without copying.
#[no_mangle]
pub extern "C" fn drm_put_raw(user: *mut c_void, src: *const c_void, size: u32) -> c_int {
    if user.is_null() || size == 0 {
        return 0;
    }
    drm_copy_out(user, src, size)
}
