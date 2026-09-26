//! x86 helpers used by locks: `pause` for spin-wait backoff, `cli`/`sti` and EFLAGS
//! snapshot/restore for [`crate::spinlock::irq_spinlock_t`]. All `unsafe` assembly is
//! isolated here.

#[inline(always)]
pub fn pause_cpu() {
    // SAFETY: `pause` is a hint instruction defined for every x86 CPU this
    // kernel runs on (a plain `rep nop` on parts without SSE2); it touches no
    // memory and no flags, so it cannot corrupt the spin loop.
    unsafe {
        core::arch::asm!("pause", options(nomem, nostack, preserves_flags));
    }
}

#[inline(always)]
pub fn interrupts_disable() {
    // SAFETY: `cli` only clears IF on the current CPU and writes no memory; the
    // caller is a lock primitive that must run this critical section with
    // maskable interrupts off.
    unsafe {
        core::arch::asm!("cli", options(nomem, nostack, preserves_flags));
    }
}

#[inline(always)]
pub fn interrupts_enable() {
    // SAFETY: `sti` only sets IF on the current CPU and writes no memory; the
    // caller has already restored the lock's saved state and expects interrupts
    // to become enabled.
    unsafe {
        core::arch::asm!("sti", options(nomem, nostack, preserves_flags));
    }
}

#[inline(always)]
pub fn eflags_read() -> u32 {
    // SAFETY: `pushfd` pushes EFLAGS onto the (valid) kernel stack and `pop`
    // reads it back into a register; both are unprivileged and the sequence is
    // stack-balanced, so it only reports the current EFLAGS value.
    unsafe {
        let flags: u32;
        core::arch::asm!("pushfd; pop {}", out(reg) flags, options(nomem, preserves_flags));
        flags
    }
}
