//! x86 helpers used by locks: `pause` for spin-wait backoff, `cli`/`sti` and EFLAGS
//! snapshot/restore for [`crate::spinlock::irq_spinlock_t`]. All `unsafe` assembly is
//! isolated here.

#[inline(always)]
pub fn pause_cpu() {
    unsafe {
        core::arch::asm!("pause", options(nomem, nostack, preserves_flags));
    }
}

#[inline(always)]
pub fn interrupts_disable() {
    unsafe {
        core::arch::asm!("cli", options(nomem, nostack, preserves_flags));
    }
}

#[inline(always)]
pub fn interrupts_enable() {
    unsafe {
        core::arch::asm!("sti", options(nomem, nostack, preserves_flags));
    }
}

#[inline(always)]
pub fn eflags_read() -> u32 {
    unsafe {
        let flags: u32;
        core::arch::asm!("pushfd; pop {}", out(reg) flags, options(nomem, nostack));
        flags
    }
}
