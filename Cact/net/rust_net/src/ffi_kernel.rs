//! Kernel services used by the Rust network stack: heap, logging, scheduler hooks, VFS, I/O ports.
//!
//! All symbols are implemented in C and linked into the final kernel image.

use core::cell::SyncUnsafeCell;
use core::ffi::{c_char, c_int, c_void};

use crate::types::{MacAddr, Semaphore, Skb, VfsNode};

unsafe extern "C" {
    pub fn kmalloc(size: usize) -> *mut c_void;
    pub fn kmalloc_aligned(size: usize, align: u32) -> *mut c_void;
    pub fn kfree(ptr: *mut c_void);

    pub fn printk(s: *mut c_char);
    pub fn printk_color(s: *mut c_char, color: u32);
    pub fn printk_hex(v: u32);

    pub fn itoa(v: c_int, out: *mut c_char);

    pub fn sema_init(s: *mut Semaphore, val: c_int);
    pub fn down(s: *mut Semaphore);
    pub fn up(s: *mut Semaphore);

    pub fn create_task(entry: extern "C" fn()) -> *mut c_void;
    pub fn timer_ticks_get() -> u32;
    pub fn sched_sleep_ticks(ticks: u32);

    pub fn read_vfs(node: *mut VfsNode, off: u32, size: u32, buf: *mut c_char) -> c_int;
    pub fn write_vfs(node: *mut VfsNode, off: u32, size: u32, buf: *mut c_char) -> c_int;
    pub fn open_vfs(node: *mut VfsNode);
    pub fn close_vfs(node: *mut VfsNode);

    pub fn inb(port: u16) -> u8;
    pub fn outw(port: u16, data: u16);

    pub static terminal_fg_pid: SyncUnsafeCell<u32>;
}

pub const LOG_OK: c_int = 0;
pub const LOG_WARN: c_int = 1;
pub const LOG_ERROR: c_int = 2;
pub const LOG_FAIL: c_int = 3;

/// `msg` must be a static `b"...\0"` slice (NUL-terminated).  Builds a
/// KERN_SOH + level prefixed buffer and forwards it to `printk`.
#[inline]
pub fn klog_static(level: c_int, msg: &'static [u8]) {
    debug_assert!(
        msg.last().copied() == Some(0),
        "klog_static requires NUL-terminated message"
    );
    let mut buf = [0u8; 1024];
    let lvl = match level {
        0 => b'6',              // LOG_OK  -> KERN_INFO
        1 => b'4',              // LOG_WARN -> KERN_WARNING
        _ => b'3',              // LOG_ERROR/LOG_FAIL -> KERN_ERR
    };
    buf[0] = 0x01;              // KERN_SOH
    buf[1] = lvl;
    let len = core::cmp::min(msg.len().saturating_sub(1), buf.len() - 3);
    buf[2..2 + len].copy_from_slice(&msg[..len]);
    buf[2 + len] = b'\n';
    buf[3 + len] = 0;
    unsafe {
        printk(buf.as_mut_ptr().cast());
    }
}

pub fn mac_equal(a: &MacAddr, b: &MacAddr) -> bool {
    a.b == b.b
}

pub unsafe fn skb_data_ptr(skb: *mut Skb) -> *mut u8 {
    (*skb).data.as_mut_ptr().add((*skb).data_offset as usize)
}
