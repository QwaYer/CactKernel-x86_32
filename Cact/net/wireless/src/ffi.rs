//! FFI surface: the handful of C kernel services the 802.11 engine needs.
//!
//! Rust↔Rust calls (the skb helpers, `register_netdev`, `netif_rx`) go through
//! `cact_net`/`cact_mm` directly; only the genuine C kernel is declared here.

use core::ffi::{c_char, c_int, c_void};

extern "C" {
    /// Validate a userspace pointer/size pair before a copy (see `validate.h`).
    pub fn validate_user_ptr(ptr: *const c_void, size: u32) -> c_int;
    pub fn copy_from_user(dst: *mut c_void, src: *const c_void, size: u32) -> c_int;
    pub fn copy_to_user(dst: *mut c_void, src: *const c_void, size: u32) -> c_int;
    pub fn ktime_get_usec() -> u64;
    pub fn ktime_busy_wait_us(us: u64);
    pub fn unregister_chrdev(name: *const c_char) -> c_int;
}

/// Mirrors `devfs_driver_t` in `Cact/fs/vfs/devfs/devfs.h` (7 slots, 28 bytes).
/// The `ctl`/`status` slots are legacy but keep their positions.
#[repr(C)]
pub struct DevfsDriver {
    pub read: Option<extern "C" fn(*mut c_void, u32, u32, *mut c_char) -> c_int>,
    pub write: Option<extern "C" fn(*mut c_void, u32, u32, *mut c_char) -> c_int>,
    pub ctl: Option<extern "C" fn(*mut c_void, *const c_char, u32) -> c_int>,
    pub status: Option<extern "C" fn(*mut c_void, *mut c_char, u32) -> c_int>,
    pub ioctl: Option<extern "C" fn(*mut c_void, u32, *mut c_void) -> c_int>,
    pub walk: Option<extern "C" fn(*mut c_void, *const c_char) -> *mut c_void>,
    pub readdir: Option<extern "C" fn(*mut c_void, u32) -> *mut c_void>,
}

const _: () = assert!(core::mem::size_of::<DevfsDriver>() == 28);

extern "C" {
    pub fn register_chrdev(
        name: *const c_char,
        flags: u32,
        drv: *mut DevfsDriver,
        drv_priv: *mut c_void,
    ) -> *mut c_void;
}

/// `DEVFS_F_CHAR` (devfs.h).
pub const DEVFS_F_CHAR: u32 = 0x04;

