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

#[no_mangle]
pub unsafe extern "C" fn spinlock_init(lock: *mut spinlock_t) {
    (*lock).init();
}

#[no_mangle]
pub unsafe extern "C" fn spinlock_acquire(lock: *mut spinlock_t) {
    (*lock).acquire();
}

#[no_mangle]
pub unsafe extern "C" fn spinlock_release(lock: *mut spinlock_t) {
    (*lock).release();
}

#[no_mangle]
pub unsafe extern "C" fn irq_spinlock_init(lock: *mut irq_spinlock_t) {
    (*lock).init();
}

#[no_mangle]
pub unsafe extern "C" fn irq_spinlock_acquire(lock: *mut irq_spinlock_t) {
    (*lock).acquire();
}

#[no_mangle]
pub unsafe extern "C" fn irq_spinlock_release(lock: *mut irq_spinlock_t) {
    (*lock).release();
}
