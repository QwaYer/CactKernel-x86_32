use core::cell::UnsafeCell;
use core::sync::atomic::{AtomicBool, AtomicU32, Ordering};

pub use alloc::sync::Arc;

#[cfg(target_arch = "x86")]
unsafe fn local_irq_save() -> u32 {
    let flags: u32;
    core::arch::asm!("pushf; pop {0}; cli", out(reg) flags, options(nostack, preserves_flags));
    flags
}

#[cfg(target_arch = "x86")]
unsafe fn local_irq_restore(flags: u32) {
    core::arch::asm!("push {0}; popf", in(reg) flags, options(nostack));
}

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

pub struct OnceLock<T> {
    data: UnsafeCell<Option<T>>,
    once: Once,
}

unsafe impl<T: Send + Sync> Sync for OnceLock<T> {}
unsafe impl<T: Send> Send for OnceLock<T> {}

impl<T> OnceLock<T> {
    pub const fn new() -> OnceLock<T> {
        OnceLock {
            data: UnsafeCell::new(None),
            once: Once::new(),
        }
    }

    pub fn get(&self) -> Option<&T> {
        if self.once.is_completed() {
            unsafe { (*self.data.get()).as_ref() }
        } else {
            None
        }
    }

    pub fn set(&self, value: T) -> Result<(), T> {
        if self.once.is_completed() {
            return Err(value);
        }
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

unsafe impl<T: Send + ?Sized> Send for Mutex<T> {}
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
        let flags = unsafe { local_irq_save() };
        while self
            .locked
            .compare_exchange(false, true, Ordering::Acquire, Ordering::Relaxed)
            .is_err()
        {
            core::hint::spin_loop();
        }
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
        unsafe { &*self.mutex.data.get() }
    }
}

impl<T: ?Sized> core::ops::DerefMut for MutexGuard<'_, T> {
    fn deref_mut(&mut self) -> &mut T {
        unsafe { &mut *self.mutex.data.get() }
    }
}

impl<T: ?Sized> Drop for MutexGuard<'_, T> {
    fn drop(&mut self) {
        let flags = unsafe { *self.mutex.irq_flags.get() };
        self.mutex.locked.store(false, Ordering::Release);
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

unsafe impl<T: Send + ?Sized> Send for RwLock<T> {}
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
        let flags = unsafe { local_irq_save() };
        loop {
            let s = self.state.load(Ordering::Relaxed);
            if s & WRITE_BIT == 0 {
                if self
                    .state
                    .compare_exchange(s, s + 1, Ordering::Acquire, Ordering::Relaxed)
                    .is_ok()
                {
                    break;
                }
            }
            core::hint::spin_loop();
        }
        unsafe { *self.irq_flags.get() = flags };
        RwLockReadGuard { lock: self }
    }

    pub fn write(&self) -> RwLockWriteGuard<'_, T> {
        let flags = unsafe { local_irq_save() };
        while self
            .state
            .compare_exchange(0, WRITE_BIT, Ordering::Acquire, Ordering::Relaxed)
            .is_err()
        {
            core::hint::spin_loop();
        }
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
        unsafe { &*self.lock.data.get() }
    }
}

impl<T: ?Sized> Drop for RwLockReadGuard<'_, T> {
    fn drop(&mut self) {
        let flags = unsafe { *self.lock.irq_flags.get() };
        self.lock.state.fetch_sub(1, Ordering::Release);
        unsafe { local_irq_restore(flags) };
    }
}

pub struct RwLockWriteGuard<'a, T: ?Sized + 'a> {
    lock: &'a RwLock<T>,
}

impl<T: ?Sized> core::ops::Deref for RwLockWriteGuard<'_, T> {
    type Target = T;
    fn deref(&self) -> &T {
        unsafe { &*self.lock.data.get() }
    }
}

impl<T: ?Sized> core::ops::DerefMut for RwLockWriteGuard<'_, T> {
    fn deref_mut(&mut self) -> &mut T {
        unsafe { &mut *self.lock.data.get() }
    }
}

impl<T: ?Sized> Drop for RwLockWriteGuard<'_, T> {
    fn drop(&mut self) {
        let flags = unsafe { *self.lock.irq_flags.get() };
        self.lock.state.store(0, Ordering::Release);
        unsafe { local_irq_restore(flags) };
    }
}
