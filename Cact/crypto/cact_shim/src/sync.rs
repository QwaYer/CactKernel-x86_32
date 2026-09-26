use core::cell::UnsafeCell;
use core::sync::atomic::{AtomicBool, AtomicU32, Ordering};

pub use alloc::sync::Arc;

#[cfg(target_arch = "x86")]
unsafe fn local_irq_save() -> u32 {
    let flags: u32;
    // SAFETY: the kernel runs in ring 0, so `cli`/`pushf` are legal and cannot
    // fault; the `pushf`/`pop` pair is stack-balanced, so `nostack` holds.
    unsafe {
        core::arch::asm!("pushf; pop {0}; cli", out(reg) flags, options(nostack, preserves_flags));
    }
    flags
}

#[cfg(target_arch = "x86")]
unsafe fn local_irq_restore(flags: u32) {
    // SAFETY: `flags` is a word previously returned by `local_irq_save`, so
    // `popf` only restores known-good ring-0 flags; `push`/`popf` is
    // stack-balanced, so `nostack` holds.
    unsafe {
        core::arch::asm!("push {0}; popf", in(reg) flags, options(nostack));
    }
}

// Non-x86 fallbacks exist so this crate (and cact_crypto with it) can be built
// on a host for testing.  The kernel itself is x86-only and always takes the
// asm path above; there, the locks really do disable interrupts.
#[cfg(not(target_arch = "x86"))]
unsafe fn local_irq_save() -> u32 {
    0
}

#[cfg(not(target_arch = "x86"))]
unsafe fn local_irq_restore(_flags: u32) {}

pub struct Once {
    done: AtomicBool,
}

impl Once {
    pub const fn new() -> Once {
        Once {
            done: AtomicBool::new(false),
        }
    }

    pub fn call_once<F>(&self, f: F)
    where
        F: FnOnce(),
    {
        if !self.done.load(Ordering::Acquire) {
            f();
            self.done.store(true, Ordering::Release);
        }
    }

    pub fn is_completed(&self) -> bool {
        self.done.load(Ordering::Acquire)
    }
}

impl Default for Once {
    fn default() -> Self {
        Self::new()
    }
}

pub struct OnceLock<T> {
    data: UnsafeCell<Option<T>>,
    once: Once,
}

// SAFETY: the value is only published after `Once` completes, whose Release
// store is paired with the Acquire load in `get`, so a reader that observes
// completion sees the fully written T; T: Send + Sync makes both the
// initialising move and the shared reference sound.
unsafe impl<T: Send + Sync> Sync for OnceLock<T> {}
// SAFETY: `OnceLock<T>` owns exactly one T and no other thread can reach it
// before `Once` publishes it, so T: Send is enough to move the whole cell.
unsafe impl<T: Send> Send for OnceLock<T> {}

impl<T> Default for OnceLock<T> {
    fn default() -> Self {
        Self::new()
    }
}

impl<T> OnceLock<T> {
    pub const fn new() -> OnceLock<T> {
        OnceLock {
            data: UnsafeCell::new(None),
            once: Once::new(),
        }
    }

    pub fn get(&self) -> Option<&T> {
        if self.once.is_completed() {
            // SAFETY: `is_completed` returned true, so the Acquire load in `Once`
            // synchronises with the Release store made after the write to `data`;
            // the `Option<T>` is therefore initialised and no `&mut` to it exists
            // while the returned reference is live.
            unsafe { (*self.data.get()).as_ref() }
        } else {
            None
        }
    }

    pub fn set(&self, value: T) -> Result<(), T> {
        if self.once.is_completed() {
            return Err(value);
        }
        // SAFETY: the `Once` is still incomplete, so the cell has never been
        // observed and this is the single initialising write; it is published by
        // the Release in `call_once` below.
        unsafe { *self.data.get() = Some(value) };
        self.once.call_once(|| {});
        Ok(())
    }

    pub fn get_or_init<F>(&self, f: F) -> &T
    where
        F: FnOnce() -> T,
    {
        if !self.once.is_completed() {
            let val = f();
            // SAFETY: the `Once` is still incomplete, so the cell is unobserved
            // and this is the single initialising write; `call_once` below
            // publishes it with a Release store.
            unsafe { *self.data.get() = Some(val) };
            self.once.call_once(|| {});
        }
        self.get().unwrap()
    }
}

/// Interrupt-safe spinlock Mutex.
///
/// Disables local interrupts on lock, restores on unlock.
pub struct Mutex<T: ?Sized> {
    locked: AtomicBool,
    irq_flags: UnsafeCell<u32>,
    data: UnsafeCell<T>,
}

// SAFETY: `Mutex` moves its T to whichever thread acquires the lock, and the
// `UnsafeCell` is only accessed while `locked` is held, so T: Send suffices.
unsafe impl<T: Send + ?Sized> Send for Mutex<T> {}
// SAFETY: `&Mutex<T>` only exposes T through `lock()`, which serialises every
// access with the `locked` compare-exchange, so sharing requires only T: Send.
unsafe impl<T: Send + ?Sized> Sync for Mutex<T> {}

impl<T> Mutex<T> {
    pub const fn new(t: T) -> Mutex<T> {
        Mutex {
            locked: AtomicBool::new(false),
            irq_flags: UnsafeCell::new(0),
            data: UnsafeCell::new(t),
        }
    }

    pub fn lock(&self) -> MutexGuard<'_, T> {
        // SAFETY: `local_irq_save` is an asm wrapper with no preconditions other
        // than running on x86 in kernel mode.
        let flags = unsafe { local_irq_save() };
        while self
            .locked
            .compare_exchange(false, true, Ordering::Acquire, Ordering::Relaxed)
            .is_err()
        {
            core::hint::spin_loop();
        }
        // SAFETY: this thread acquired the lock with the CAS above, so it has
        // exclusive access to `irq_flags` until the matching guard is dropped.
        unsafe { *self.irq_flags.get() = flags };
        MutexGuard { mutex: self }
    }
}

pub struct MutexGuard<'a, T: ?Sized + 'a> {
    mutex: &'a Mutex<T>,
}

impl<T: ?Sized> core::ops::Deref for MutexGuard<'_, T> {
    type Target = T;
    fn deref(&self) -> &T {
        // SAFETY: the guard only exists while `self.mutex` is locked, so no
        // other thread holds a `&`/`&mut` to `data`; the borrow is tied to the
        // guard's `&self` and cannot outlive the lock.
        unsafe { &*self.mutex.data.get() }
    }
}

impl<T: ?Sized> core::ops::DerefMut for MutexGuard<'_, T> {
    fn deref_mut(&mut self) -> &mut T {
        // SAFETY: `&mut self` proves this guard is the unique accessor, and the
        // mutex is still locked, so no other thread can alias `data`; the
        // borrow is tied to the guard and cannot outlive the lock.
        unsafe { &mut *self.mutex.data.get() }
    }
}

impl<T: ?Sized> Drop for MutexGuard<'_, T> {
    fn drop(&mut self) {
        // SAFETY: this thread wrote `irq_flags` in `lock` and has held the lock
        // ever since, so this is the matching read.
        let flags = unsafe { *self.mutex.irq_flags.get() };
        self.mutex.locked.store(false, Ordering::Release);
        // SAFETY: `flags` is the value captured by the matching
        // `local_irq_save` in `lock`.
        unsafe { local_irq_restore(flags) };
    }
}

impl<T: ?Sized + fmt::Debug> fmt::Debug for Mutex<T> {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "Mutex")
    }
}

use core::fmt;

/// Reader-writer lock using a reader count (AtomicU32).
///
/// Multiple concurrent readers are allowed; writers get exclusive access.
/// Interrupt-safe: disables local interrupts on lock, restores on unlock.
pub struct RwLock<T: ?Sized> {
    /// Bit 31: write locked. Bits 0-30: reader count.
    state: AtomicU32,
    irq_flags: UnsafeCell<u32>,
    data: UnsafeCell<T>,
}

const WRITE_BIT: u32 = 1 << 31;

// SAFETY: `RwLock` transfers T with the lock and its `UnsafeCell` is reachable
// only through the guards, so T: Send suffices.
unsafe impl<T: Send + ?Sized> Send for RwLock<T> {}
// SAFETY: `&RwLock<T>` exposes T only via `read`/`write`, and both are
// serialised by the `state` CAS (one writer or many readers), so sharing
// requires only T: Send.
unsafe impl<T: Send + ?Sized> Sync for RwLock<T> {}

impl<T> RwLock<T> {
    pub const fn new(t: T) -> RwLock<T> {
        RwLock {
            state: AtomicU32::new(0),
            irq_flags: UnsafeCell::new(0),
            data: UnsafeCell::new(t),
        }
    }

    pub fn read(&self) -> RwLockReadGuard<'_, T> {
        // SAFETY: `local_irq_save` is an asm wrapper with no preconditions other
        // than running on x86 in kernel mode.
        let flags = unsafe { local_irq_save() };
        loop {
            let s = self.state.load(Ordering::Relaxed);
            if s & WRITE_BIT == 0
                && self
                    .state
                    .compare_exchange(s, s + 1, Ordering::Acquire, Ordering::Relaxed)
                    .is_ok()
            {
                break;
            }
            core::hint::spin_loop();
        }
        // SAFETY: the reader slot was just acquired above; interrupts are off on
        // this CPU and the cell is a plain `UnsafeCell` that never has a `&mut`
        // formed against it, so this store records the flags for the matching
        // `drop`.
        unsafe { *self.irq_flags.get() = flags };
        RwLockReadGuard { lock: self }
    }

    pub fn write(&self) -> RwLockWriteGuard<'_, T> {
        // SAFETY: `local_irq_save` is an asm wrapper with no preconditions other
        // than running on x86 in kernel mode.
        let flags = unsafe { local_irq_save() };
        while self
            .state
            .compare_exchange(0, WRITE_BIT, Ordering::Acquire, Ordering::Relaxed)
            .is_err()
        {
            core::hint::spin_loop();
        }
        // SAFETY: this thread acquired the write lock with the CAS above, so it
        // is the only holder of the lock and has exclusive access to `irq_flags`
        // until the guard is dropped.
        unsafe { *self.irq_flags.get() = flags };
        RwLockWriteGuard { lock: self }
    }
}

pub struct RwLockReadGuard<'a, T: ?Sized + 'a> {
    lock: &'a RwLock<T>,
}

impl<T: ?Sized> core::ops::Deref for RwLockReadGuard<'_, T> {
    type Target = T;
    fn deref(&self) -> &T {
        // SAFETY: the guard only exists while this thread holds a read slot
        // (write bit clear), so no writer can form `&mut` to `data`, and the
        // returned borrow is tied to the guard and cannot outlive the lock.
        unsafe { &*self.lock.data.get() }
    }
}

impl<T: ?Sized> Drop for RwLockReadGuard<'_, T> {
    fn drop(&mut self) {
        // SAFETY: this thread stored `irq_flags` in `read` and has held a read
        // slot ever since, so this is the matching read.
        let flags = unsafe { *self.lock.irq_flags.get() };
        self.lock.state.fetch_sub(1, Ordering::Release);
        // SAFETY: `flags` is the value captured by the matching
        // `local_irq_save` in `read`.
        unsafe { local_irq_restore(flags) };
    }
}

pub struct RwLockWriteGuard<'a, T: ?Sized + 'a> {
    lock: &'a RwLock<T>,
}

impl<T: ?Sized> core::ops::Deref for RwLockWriteGuard<'_, T> {
    type Target = T;
    fn deref(&self) -> &T {
        // SAFETY: the guard only exists while this thread holds the write lock
        // (write bit set), so it is the only accessor of `data`, and the borrow
        // is tied to the guard and cannot outlive the lock.
        unsafe { &*self.lock.data.get() }
    }
}

impl<T: ?Sized> core::ops::DerefMut for RwLockWriteGuard<'_, T> {
    fn deref_mut(&mut self) -> &mut T {
        // SAFETY: `&mut self` proves this guard is the unique accessor and the
        // write lock is still held, so no other thread can alias `data`; the
        // borrow is tied to the guard and cannot outlive the lock.
        unsafe { &mut *self.lock.data.get() }
    }
}

impl<T: ?Sized> Drop for RwLockWriteGuard<'_, T> {
    fn drop(&mut self) {
        // SAFETY: this thread stored `irq_flags` in `write` while holding the
        // write lock, so this is the matching read.
        let flags = unsafe { *self.lock.irq_flags.get() };
        self.lock.state.store(0, Ordering::Release);
        // SAFETY: `flags` is the value captured by the matching
        // `local_irq_save` in `write`.
        unsafe { local_irq_restore(flags) };
    }
}
