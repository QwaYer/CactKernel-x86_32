//! Ticketless spinlock (`pause` while spinning) and IRQ-disabling variant that saves
//! the IF flag so `release` restores interrupts only if they were enabled on `acquire`.

use core::sync::atomic::{AtomicU32, Ordering};

use crate::hal;

#[repr(C)]
pub struct spinlock_t {
    pub locked: AtomicU32,
}

impl spinlock_t {
    pub const fn new() -> Self {
        Self { locked: AtomicU32::new(0) }
    }

    pub fn init(&mut self) {
        self.locked.store(0, Ordering::Relaxed);
    }

    pub fn acquire(&mut self) {
        loop {
            if self
                .locked
                .compare_exchange(0, 1, Ordering::Acquire, Ordering::Relaxed)
                .is_ok()
            {
                return;
            }
            while self.locked.load(Ordering::Relaxed) != 0 {
                hal::pause_cpu();
            }
        }
    }

    pub fn release(&mut self) {
        self.locked.store(0, Ordering::Release);
    }
}

impl Default for spinlock_t {
    fn default() -> Self {
        Self::new()
    }
}

#[repr(C)]
pub struct irq_spinlock_t {
    pub spin:        spinlock_t,
    pub saved_flags: u32,
}

impl irq_spinlock_t {
    pub const fn new() -> Self {
        Self {
            spin:        spinlock_t::new(),
            saved_flags: 0,
        }
    }

    pub fn init(&mut self) {
        self.spin.init();
        self.saved_flags = 0;
    }

    pub fn acquire(&mut self) {
        let flags = hal::eflags_read();
        hal::interrupts_disable();
        self.spin.acquire();
        self.saved_flags = flags;
    }

    pub fn release(&mut self) {
        let flags = self.saved_flags;
        self.spin.release();
        if flags & (1 << 9) != 0 {
            hal::interrupts_enable();
        }
    }
}

impl Default for irq_spinlock_t {
    fn default() -> Self {
        Self::new()
    }
}

/// # Safety
///
/// `lock` must be non-null and properly aligned, and must point to live storage
/// for a `spinlock_t` that the caller may access exclusively for this call.
#[no_mangle]
pub unsafe extern "C" fn spin_lock_init(lock: *mut spinlock_t) {
    // SAFETY: the caller contract (see # Safety) makes `lock` a valid, aligned,
    // exclusively accessible pointer for the duration of the call.
    unsafe { (*lock).init() };
}

/// # Safety
///
/// `lock` must be non-null and properly aligned, must point to an initialised
/// `spinlock_t` that stays live across the call, and must not be acquired
/// concurrently by any other CPU.
#[no_mangle]
pub unsafe extern "C" fn spin_lock(lock: *mut spinlock_t) {
    // SAFETY: the caller contract (see # Safety) makes `lock` a valid, aligned
    // pointer to a live lock that this call is allowed to acquire.
    unsafe { (*lock).acquire() };
}

/// # Safety
///
/// `lock` must be non-null and properly aligned, must point to a lock currently
/// held by the calling task, and must stay live across the call.
#[no_mangle]
pub unsafe extern "C" fn spin_unlock(lock: *mut spinlock_t) {
    // SAFETY: the caller contract (see # Safety) makes `lock` a valid, aligned
    // pointer to a live lock owned by this task.
    unsafe { (*lock).release() };
}

/// # Safety
///
/// `lock` must be non-null and properly aligned, and must point to live storage
/// for an `irq_spinlock_t` that the caller may access exclusively for this call.
#[no_mangle]
pub unsafe extern "C" fn irq_spinlock_init(lock: *mut irq_spinlock_t) {
    // SAFETY: the caller contract (see # Safety) makes `lock` a valid, aligned,
    // exclusively accessible pointer for the duration of the call.
    unsafe { (*lock).init() };
}

/// # Safety
///
/// `lock` must be non-null and properly aligned, must point to an initialised
/// `irq_spinlock_t` that stays live across the call, and must not be acquired
/// concurrently by any other CPU.
#[no_mangle]
pub unsafe extern "C" fn irq_spinlock_acquire(lock: *mut irq_spinlock_t) {
    // SAFETY: the caller contract (see # Safety) makes `lock` a valid, aligned
    // pointer to a live lock that this call is allowed to acquire.
    unsafe { (*lock).acquire() };
}

/// # Safety
///
/// `lock` must be non-null and properly aligned, must point to an
/// `irq_spinlock_t` currently held (and previously acquired on this CPU), and
/// must stay live across the call.
#[no_mangle]
pub unsafe extern "C" fn irq_spinlock_release(lock: *mut irq_spinlock_t) {
    // SAFETY: the caller contract (see # Safety) makes `lock` a valid, aligned
    // pointer to a live lock owned by this CPU.
    unsafe { (*lock).release() };
}

// Linux-style IRQ-safe spinlocks: flags live in a caller-local variable.
//   unsigned long flags; spin_lock_irqsave(&lock, &flags);
//   ... spin_unlock_irqrestore(&lock, flags);
/// # Safety
///
/// `lock` must be non-null and properly aligned, must point to an initialised
/// `spinlock_t` that stays live across the call, and must not be concurrently
/// acquired by another CPU. `flags` may be null (the saved EFLAGS are then
/// discarded); if non-null it must point to a writable `u32`.
#[no_mangle]
pub unsafe extern "C" fn spin_lock_irqsave(lock: *mut spinlock_t, flags: *mut u32) {
    let saved = hal::eflags_read();
    hal::interrupts_disable();
    // SAFETY: the caller contract (see # Safety) makes `lock` a valid, aligned
    // pointer to a live lock that this call is allowed to acquire.
    unsafe { (*lock).acquire() };
    if !flags.is_null() {
        // SAFETY: the check above rules out null and the caller contract (see
        // # Safety) guarantees `flags` points to a writable `u32`.
        unsafe { *flags = saved };
    }
}

/// # Safety
///
/// `lock` must be non-null and properly aligned, must point to a lock currently
/// held by the calling task, and must stay live across the call. `flags` must be
/// the EFLAGS value captured by the matching `spin_lock_irqsave`.
#[no_mangle]
pub unsafe extern "C" fn spin_unlock_irqrestore(lock: *mut spinlock_t, flags: u32) {
    // SAFETY: the caller contract (see # Safety) makes `lock` a valid, aligned
    // pointer to a live lock owned by this task.
    unsafe { (*lock).release() };
    if flags & (1 << 9) != 0 {
        hal::interrupts_enable();
    }
}
