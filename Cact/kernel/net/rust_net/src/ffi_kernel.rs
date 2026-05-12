use core::ffi::{c_char, c_int, c_void};

use crate::types::{MacAddr, Semaphore, Skb, VfsNode};

unsafe extern "C" {
    pub fn kmalloc(size: usize) -> *mut c_void;
    pub fn kmalloc_aligned(size: usize, align: u32) -> *mut c_void;
    pub fn kfree_heap(ptr: *mut c_void);

    pub fn kprint(s: *mut c_char);
    pub fn kprint_color(s: *mut c_char, color: u32);
    pub fn kprint_hex(v: u32);
    pub fn klog(level: c_int, s: *const c_char);

    pub fn itoa(v: c_int, out: *mut c_char);

    pub fn sema_init(s: *mut Semaphore, val: c_int);
    pub fn sema_down(s: *mut Semaphore);
    pub fn sema_up(s: *mut Semaphore);

    pub fn create_task(entry: extern "C" fn()) -> *mut c_void;
    pub fn timer_ticks_get() -> u32;
    pub fn sched_sleep_ticks(ticks: u32);

    pub fn read_vfs(node: *mut VfsNode, off: u32, size: u32, buf: *mut c_char) -> c_int;
    pub fn write_vfs(node: *mut VfsNode, off: u32, size: u32, buf: *mut c_char) -> c_int;
    pub fn open_vfs(node: *mut VfsNode);
    pub fn close_vfs(node: *mut VfsNode);

    pub fn port_byte_in(port: u16) -> u8;
    pub fn port_word_out(port: u16, data: u16);

    pub static mut terminal_fg_pid: u32;
}

pub const LOG_OK: c_int = 0;
pub const LOG_WARN: c_int = 1;
pub const LOG_ERROR: c_int = 2;
pub const LOG_FAIL: c_int = 3;

/// `msg` must be a static `b"...\0"` slice (NUL-terminated for C `klog`).
#[inline]
pub fn klog_static(level: c_int, msg: &'static [u8]) {
    debug_assert!(
        msg.last().copied() == Some(0),
        "klog_static requires NUL-terminated message"
    );
    unsafe {
        klog(level, msg.as_ptr().cast());
    }
}

pub fn mac_equal(a: &MacAddr, b: &MacAddr) -> bool {
    a.b == b.b
}

pub unsafe fn skb_data_ptr(skb: *mut Skb) -> *mut u8 {
    (*skb).data.as_mut_ptr().add((*skb).data_offset as usize)
}
