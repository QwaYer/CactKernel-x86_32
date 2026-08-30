//! Safe wrappers for kernel primitives.
//!
//! This module provides abstractions that encapsulate `unsafe` operations
//! behind safe interfaces, following the kernel's synchronization protocol
//! (IRQ-safe spinlocks) to guarantee memory safety at runtime.

use core::cell::UnsafeCell;

// ---------------------------------------------------------------------------
// KStatic — a `Sync` wrapper for mutable statics
// ---------------------------------------------------------------------------

/// A `Sync`-safe wrapper around `UnsafeCell<T>` for kernel mutable statics.
///
/// # Safety contract
///
/// The kernel uses `IrqSpinlock` to serialize access to shared mutable state.
/// Any code that calls [`KStatic::get_mut`] or [`KStatic::as_ptr`] must
/// already hold the appropriate spinlock (or be in a single-threaded boot
/// context where no concurrent access is possible).
pub struct KStatic<T>(UnsafeCell<T>);

// SAFETY: Access is guarded by IrqSpinlock at every call site.
unsafe impl<T> Sync for KStatic<T> {}

// SAFETY: Access is guarded by IrqSpinlock at every call site.
unsafe impl<T> Send for KStatic<T> {}

impl<T> KStatic<T> {
    /// Create a new `KStatic` with the given initial value.
    pub const fn new(val: T) -> Self {
        KStatic(UnsafeCell::new(val))
    }

    /// Obtain a mutable reference to the inner value.
    ///
    /// The caller must ensure proper synchronization (typically by holding
    /// the associated `IrqSpinlock`).
    pub fn get_mut(&self) -> &mut T {
        // SAFETY: guaranteed by the kernel's spinlock protocol.
        unsafe { &mut *self.0.get() }
    }

    /// Obtain a raw mutable pointer to the inner value.
    ///
    /// Useful for passing to C FFI functions that expect `*mut T`.
    pub fn as_ptr(&self) -> *mut T {
        self.0.get()
    }
}

// ---------------------------------------------------------------------------
// Helper: zero a page safely
// ---------------------------------------------------------------------------

use crate::ffi::PAGE_SIZE;

/// Zero-fill an entire 4 KiB page starting at `ptr`.
///
/// Does nothing if `ptr` is null.
pub fn zero_page(ptr: *mut u8) {
    if !ptr.is_null() {
        // SAFETY: caller guarantees `ptr` points to a valid 4 KiB page.
        unsafe { core::ptr::write_bytes(ptr, 0, PAGE_SIZE as usize) };
    }
}

// ---------------------------------------------------------------------------
// Safe wrappers around inline-asm / FFI helpers
// ---------------------------------------------------------------------------

use crate::ffi::{
    irq_spinlock_acquire, irq_spinlock_release, itoa, printk,
    read_cr2, tlb_flush, tlb_flush_all, get_current_pd,
};

/// Read the CR2 register (page-fault linear address).
pub fn read_cr2_val() -> u32 {
    read_cr2()
}

/// Flush a single TLB entry for `vaddr`.
pub fn flush_tlb(vaddr: u32) {
    tlb_flush(vaddr)
}

/// Flush the entire TLB by reloading CR3.
pub fn flush_tlb_all() {
    tlb_flush_all()
}

/// Return the current page-directory pointer from CR3.
pub fn current_page_dir() -> *mut u32 {
    get_current_pd()
}

/// Print a null-terminated byte string via the kernel console.
pub fn kprint_str(s: *const u8) {
    if !s.is_null() {
        // SAFETY: caller guarantees `s` is a valid null-terminated string.
        unsafe { printk(s) };
    }
}

/// Print a signed integer.
pub fn kprint_int(n: i32) {
    let mut buf = [0u8; 16];
    unsafe {
        itoa(n, buf.as_mut_ptr());
        printk(buf.as_ptr());
    }
}

/// Log a message at the given level (KERN_SOH + level prefix).
pub fn klog_msg(level: u32, msg: *const u8) {
    if !msg.is_null() {
        let mut buf = [0u8; 1024];
        let lvl = match level {
            0 => b'6',          // LOG_OK  -> KERN_INFO
            1 => b'4',          // LOG_WARN -> KERN_WARNING
            _ => b'3',          // LOG_ERROR/LOG_FAIL -> KERN_ERR
        };
        buf[0] = 0x01;          // KERN_SOH
        buf[1] = lvl;
        let mut n = 2;
        unsafe {
            while *msg.add(n - 2) != 0 && n < buf.len() - 2 {
                buf[n] = *msg.add(n - 2);
                n += 1;
            }
        }
        buf[n] = b'\n';
        buf[n + 1] = 0;
        // SAFETY: `buf` is a valid null-terminated string.
        unsafe { printk(buf.as_ptr()) };
    }
}

/// Acquire an IRQ-safe spinlock.
pub fn lock_acquire(lock: *mut crate::ffi::IrqSpinlock) {
    if !lock.is_null() {
        // SAFETY: the spinlock protocol ensures correctness.
        unsafe { irq_spinlock_acquire(lock) };
    }
}

/// Release an IRQ-safe spinlock.
pub fn lock_release(lock: *mut crate::ffi::IrqSpinlock) {
    if !lock.is_null() {
        // SAFETY: must be called only after a matching `lock_acquire`.
        unsafe { irq_spinlock_release(lock) };
    }
}
