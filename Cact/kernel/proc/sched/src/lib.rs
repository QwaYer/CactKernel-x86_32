//! Process scheduler: tasks, MLFQ, timer wheel, and a `sync` re-export of `cact_sync`.
//!
//! `pub mod sync` exists so C/Rust link units can refer to one `sched` object file for
//! both scheduling logic and primitives (`mutex_t`, `spinlock_t`, …). Compile-time
//! assertions below keep `TaskStruct` identical to the C layout.

#![no_std]
#![feature(sync_unsafe_cell)]
#![allow(non_camel_case_types)]
#![allow(non_snake_case)]
#![allow(non_upper_case_globals)]
#![allow(clippy::missing_safety_doc)]
pub mod ffi;
pub mod task;
pub mod mlfq;
pub mod timer_wheel;

pub mod sync {
    pub use cact_sync::*;
}

#[panic_handler]
fn panic_handler(_info: &core::panic::PanicInfo) -> ! {
    unsafe {
        core::arch::asm!("cli");
        loop {
            core::arch::asm!("hlt");
        }
    }
}

const _ABI_CHECK: () = {
    use core::mem::offset_of;
    use cact_sync::task_abi::TaskStruct;

    assert!(offset_of!(TaskStruct, esp)            ==  0, "esp offset mismatch");
    assert!(offset_of!(TaskStruct, pid)            ==  4, "pid offset mismatch");
    assert!(offset_of!(TaskStruct, state)          ==  8, "state offset mismatch");
    assert!(offset_of!(TaskStruct, is_kernel)      == 12, "is_kernel offset mismatch");
    assert!(offset_of!(TaskStruct, _pad0)          == 13, "_pad0 offset mismatch");
    assert!(offset_of!(TaskStruct, stack_base)     == 16, "stack_base offset mismatch");
    assert!(offset_of!(TaskStruct, ustack_phys)    == 20, "ustack_phys offset mismatch");
    assert!(offset_of!(TaskStruct, ustack_virt)    == 24, "ustack_virt offset mismatch");
    assert!(offset_of!(TaskStruct, page_directory) == 28, "page_directory offset mismatch");
    assert!(offset_of!(TaskStruct, next)           == 32, "next offset mismatch");
    assert!(offset_of!(TaskStruct, queue_next)     == 36, "queue_next offset mismatch");
    // MLFQ fields (40..52)
    assert!(offset_of!(TaskStruct, priority)       == 40, "priority offset mismatch");
    assert!(offset_of!(TaskStruct, time_slice)     == 44, "time_slice offset mismatch");
    assert!(offset_of!(TaskStruct, ticks_used)     == 48, "ticks_used offset mismatch");
    // Signals and masks (52+)
    assert!(offset_of!(TaskStruct, pending_signals) == 52, "pending_signals offset mismatch");
    assert!(offset_of!(TaskStruct, signal_mask)     == 56, "signal_mask offset mismatch");
    assert!(offset_of!(TaskStruct, saved_signal_mask) == 60, "saved_signal_mask offset mismatch");
    assert!(offset_of!(TaskStruct, in_sigsuspend)   == 64, "in_sigsuspend offset mismatch");
    assert!(offset_of!(TaskStruct, signal_handlers) == 68, "signal_handlers offset mismatch");
    // `signal_handlers`: 13 × u32 = 52 bytes → ends at 120
    assert!(offset_of!(TaskStruct, sigreturn_trampoline) == 120, "sigreturn_trampoline offset mismatch");
    // Interval timers
    assert!(offset_of!(TaskStruct, alarm_ticks)    == 124, "alarm_ticks offset mismatch");
    assert!(offset_of!(TaskStruct, itimer_value)   == 128, "itimer_value offset mismatch");
    assert!(offset_of!(TaskStruct, itimer_interval) == 132, "itimer_interval offset mismatch");
    // FD table lives on the heap (`fds` pointer)
    assert!(offset_of!(TaskStruct, fds)            == 136, "fds offset mismatch");
    // `mm`: `ProcPageTracker` (16 bytes)
    assert!(offset_of!(TaskStruct, mm)             == 140, "mm offset mismatch");
    // `mmap_table`: heap pointer (replaces the old embedded 7172-byte struct)
    assert!(offset_of!(TaskStruct, mmap_table)     == 156, "mmap_table offset mismatch");
    assert!(offset_of!(TaskStruct, dyn_ctx)        == 160, "dyn_ctx offset mismatch");
    assert!(offset_of!(TaskStruct, parent_pid)     == 164, "parent_pid offset mismatch");
    assert!(offset_of!(TaskStruct, exit_code)      == 168, "exit_code offset mismatch");
    assert!(offset_of!(TaskStruct, wait_for_pid)   == 172, "wait_for_pid offset mismatch");
    assert!(offset_of!(TaskStruct, brk_start)      == 176, "brk_start offset mismatch");
    assert!(offset_of!(TaskStruct, brk_current)    == 180, "brk_current offset mismatch");
    assert!(offset_of!(TaskStruct, sleep_until)    == 184, "sleep_until offset mismatch");
    assert!(offset_of!(TaskStruct, cwd)            == 188, "cwd offset mismatch");
    // `cwd`: 256 bytes → next field at 444
    assert!(offset_of!(TaskStruct, uid)            == 444, "uid offset mismatch");
    assert!(offset_of!(TaskStruct, gid)            == 448, "gid offset mismatch");
    assert!(offset_of!(TaskStruct, euid)           == 452, "euid offset mismatch");
    assert!(offset_of!(TaskStruct, egid)           == 456, "egid offset mismatch");
    // `shm_attachments`: 16 × `TaskShmAttach` (8 bytes each) = 128 bytes
    assert!(offset_of!(TaskStruct, shm_attachments) == 460, "shm_attachments offset mismatch");
    assert!(offset_of!(TaskStruct, wait_next)       == 588, "wait_next offset mismatch");
    assert!(offset_of!(TaskStruct, pgid)            == 592, "pgid offset mismatch");
    assert!(offset_of!(TaskStruct, sid)             == 596, "sid offset mismatch");
    assert!(offset_of!(TaskStruct, umask)           == 600, "umask offset mismatch");
    assert!(offset_of!(TaskStruct, root)             == 604, "root offset mismatch");
    assert!(offset_of!(TaskStruct, ustack_phys_extra) == 608, "ustack_phys_extra offset mismatch");
};
