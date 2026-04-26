#![no_std]
#![allow(non_camel_case_types)]
#![allow(non_snake_case)]
#![allow(non_upper_case_globals)]
#![allow(clippy::missing_safety_doc)]
pub mod ffi;
pub mod sync;
pub mod task;
pub mod mlfq;
pub mod timer_wheel;

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
    use task::TaskStruct;

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
    // Signal fields (52..)
    assert!(offset_of!(TaskStruct, pending_signals) == 52, "pending_signals offset mismatch");
    assert!(offset_of!(TaskStruct, signal_mask)     == 56, "signal_mask offset mismatch");
    assert!(offset_of!(TaskStruct, saved_signal_mask) == 60, "saved_signal_mask offset mismatch");
    assert!(offset_of!(TaskStruct, in_sigsuspend)   == 64, "in_sigsuspend offset mismatch");
    assert!(offset_of!(TaskStruct, signal_handlers) == 68, "signal_handlers offset mismatch");
    // signal_handlers: 13 * 4 = 52 bytes → ends at 120
    assert!(offset_of!(TaskStruct, sigreturn_trampoline) == 120, "sigreturn_trampoline offset mismatch");
    // Timers
    assert!(offset_of!(TaskStruct, alarm_ticks)    == 124, "alarm_ticks offset mismatch");
    assert!(offset_of!(TaskStruct, itimer_value)   == 128, "itimer_value offset mismatch");
    assert!(offset_of!(TaskStruct, itimer_interval) == 132, "itimer_interval offset mismatch");
    // FD tables: each 256 * 4 = 1024
    assert!(offset_of!(TaskStruct, fd_table)       == 136, "fd_table offset mismatch");
    assert!(offset_of!(TaskStruct, fd_offset)      == 1160, "fd_offset offset mismatch");
    assert!(offset_of!(TaskStruct, fd_flags)       == 2184, "fd_flags offset mismatch");
    assert!(offset_of!(TaskStruct, fd_cloexec)     == 3208, "fd_cloexec offset mismatch");
    // mm: ProcPageTracker = 16 bytes (4 pointers/u32)
    assert!(offset_of!(TaskStruct, mm)             == 4232, "mm offset mismatch");
    // mmap_table: 7172 bytes
    assert!(offset_of!(TaskStruct, mmap_table)     == 4248, "mmap_table offset mismatch");
    assert!(offset_of!(TaskStruct, dyn_ctx)        == 11420, "dyn_ctx offset mismatch");
    assert!(offset_of!(TaskStruct, parent_pid)     == 11424, "parent_pid offset mismatch");
    assert!(offset_of!(TaskStruct, exit_code)      == 11428, "exit_code offset mismatch");
    assert!(offset_of!(TaskStruct, wait_for_pid)   == 11432, "wait_for_pid offset mismatch");
    assert!(offset_of!(TaskStruct, brk_start)      == 11436, "brk_start offset mismatch");
    assert!(offset_of!(TaskStruct, brk_current)    == 11440, "brk_current offset mismatch");
    assert!(offset_of!(TaskStruct, sleep_until)    == 11444, "sleep_until offset mismatch");
    assert!(offset_of!(TaskStruct, cwd)            == 11448, "cwd offset mismatch");
    // cwd: 256 bytes → ends at 11704
    assert!(offset_of!(TaskStruct, uid)            == 11704, "uid offset mismatch");
    assert!(offset_of!(TaskStruct, gid)            == 11708, "gid offset mismatch");
    assert!(offset_of!(TaskStruct, euid)           == 11712, "euid offset mismatch");
    assert!(offset_of!(TaskStruct, egid)           == 11716, "egid offset mismatch");
    // shm_attachments: 16 * 8 = 128
    assert!(offset_of!(TaskStruct, shm_attachments) == 11720, "shm_attachments offset mismatch");
    assert!(offset_of!(TaskStruct, wait_next)       == 11848, "wait_next offset mismatch");
};
