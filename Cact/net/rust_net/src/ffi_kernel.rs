//! Kernel services used by the Rust network stack: heap, logging, scheduler hooks, VFS, I/O ports.
//!
//! All symbols are implemented in C and linked into the final kernel image.

use core::cell::SyncUnsafeCell;
use core::ffi::{c_char, c_int};

use crate::types::{MacAddr, Skb, VfsNode};

unsafe extern "C" {

    pub fn printk(s: *mut c_char);
    pub fn printk_color(s: *mut c_char, color: u32);
    pub fn printk_hex(v: u32);

    pub fn itoa(v: c_int, out: *mut c_char);


    pub fn timer_ticks_get() -> u32;
    pub fn ktime_get_usec() -> u64;
    pub fn ktime_busy_wait_us(us: u64);

    /// Pending, unmasked signal on the current task; blocking waits poll it so a
    /// task stuck in a socket read can still be interrupted (Ctrl+C).

    /// IRQ-saving spinlock (kernel `irq_spinlock_t`: a spin word + saved flags).

    pub fn read_vfs(node: *mut VfsNode, off: u32, size: u32, buf: *mut c_char) -> c_int;
    pub fn write_vfs(node: *mut VfsNode, off: u32, size: u32, buf: *mut c_char) -> c_int;
    pub fn open_vfs(node: *mut VfsNode);
    pub fn close_vfs(node: *mut VfsNode);

    pub fn inb(port: u16) -> u8;
    pub fn outw(port: u16, data: u16);

    pub static terminal_fg_pid: SyncUnsafeCell<u32>;
}

pub use cact_mm::kmalloc;
// The scheduler's semaphore/lock entry points are `cact_sync`'s own
// functions (Rust), called here by crate path instead of by C ABI.
pub use cact_sync::{down, irq_spinlock_acquire, irq_spinlock_release, sema_init, up};
pub use cact_mm::kmalloc_aligned;
pub use cact_mm::kfree;

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
    // SAFETY: `buf` is a local array that is NUL-terminated at `buf[3 + len]`
    // (len is capped at `buf.len() - 3`), so `printk`'s C-string contract is
    // met, and the array outlives the call.
    unsafe {
        printk(buf.as_mut_ptr().cast());
    }
}

pub fn mac_equal(a: &MacAddr, b: &MacAddr) -> bool {
    a.b == b.b
}

/// # Safety
///
/// `skb` must be non-null and point to a live, initialised [`Skb`] — one
/// returned by `skb_alloc` or received by a NIC driver — that the caller may
/// access for the duration of the call, with `data_offset` maintained within
/// `SKB_MAX_SIZE` (which every producer in this crate guarantees).
pub unsafe fn skb_data_ptr(skb: *mut Skb) -> *mut u8 {
    // SAFETY: the caller contract (see # Safety) makes `skb` a valid pointer to
    // a live `Skb` that this call borrows and does not let escape.
    let s = unsafe { &mut *skb };
    let offset = s.data_offset;
    // SAFETY: `offset` is the validated `data_offset`, which producers keep
    // below `SKB_MAX_SIZE`, so the resulting pointer stays inside `s.data`.
    unsafe { s.data.as_mut_ptr().add(offset as usize) }
}
