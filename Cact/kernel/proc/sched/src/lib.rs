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
    use cact_sync::task_abi::{ProcMeta, TaskStruct};

    assert!(offset_of!(TaskStruct, esp)             ==  0, "esp offset mismatch");
    assert!(offset_of!(TaskStruct, page_directory)  ==  4, "page_directory offset mismatch");
    assert!(offset_of!(TaskStruct, fpu_context_ptr) ==  8, "fpu_context_ptr offset mismatch");
    assert!(offset_of!(TaskStruct, pid)             == 12, "pid offset mismatch");
    assert!(offset_of!(TaskStruct, state)           == 16, "state offset mismatch");
    assert!(offset_of!(TaskStruct, is_kernel)       == 20, "is_kernel offset mismatch");
    assert!(offset_of!(TaskStruct, next)            == 24, "next offset mismatch");
    assert!(offset_of!(TaskStruct, queue_next)      == 28, "queue_next offset mismatch");
    assert!(offset_of!(TaskStruct, priority)        == 32, "priority offset mismatch");
    assert!(offset_of!(TaskStruct, time_slice)      == 36, "time_slice offset mismatch");
    assert!(offset_of!(TaskStruct, ticks_used)      == 40, "ticks_used offset mismatch");
    assert!(offset_of!(TaskStruct, proc)            == 44, "proc offset mismatch");
    assert!(core::mem::size_of::<TaskStruct>() == 48, "TaskStruct size mismatch");

    assert!(offset_of!(ProcMeta, stack_base)         ==  0, "stack_base offset mismatch");
    assert!(offset_of!(ProcMeta, ustack_phys)        ==  4, "ustack_phys offset mismatch");
    assert!(offset_of!(ProcMeta, ustack_virt)        ==  8, "ustack_virt offset mismatch");
    assert!(offset_of!(ProcMeta, ustack_phys_extra)  == 12, "ustack_phys_extra offset mismatch");
    assert!(offset_of!(ProcMeta, pending_signals)    == 24, "pending_signals offset mismatch");
    assert!(offset_of!(ProcMeta, signal_mask)        == 28, "signal_mask offset mismatch");
    assert!(offset_of!(ProcMeta, saved_signal_mask)  == 32, "saved_signal_mask offset mismatch");
    assert!(offset_of!(ProcMeta, in_sigsuspend)      == 36, "in_sigsuspend offset mismatch");
    assert!(offset_of!(ProcMeta, signal_handlers)    == 40, "signal_handlers offset mismatch");
    assert!(offset_of!(ProcMeta, sigreturn_trampoline) == 92, "sigreturn_trampoline offset mismatch");
    assert!(offset_of!(ProcMeta, alarm_ticks)        == 96, "alarm_ticks offset mismatch");
    assert!(offset_of!(ProcMeta, itimer_value)       == 100, "itimer_value offset mismatch");
    assert!(offset_of!(ProcMeta, itimer_interval)    == 104, "itimer_interval offset mismatch");
    assert!(offset_of!(ProcMeta, fds)                 == 108, "fds offset mismatch");
    assert!(offset_of!(ProcMeta, mm)                  == 112, "mm offset mismatch");
    assert!(offset_of!(ProcMeta, mmap_table)          == 128, "mmap_table offset mismatch");
    assert!(offset_of!(ProcMeta, dyn_ctx)             == 132, "dyn_ctx offset mismatch");
    assert!(offset_of!(ProcMeta, parent_pid)          == 136, "parent_pid offset mismatch");
    assert!(offset_of!(ProcMeta, exit_code)           == 140, "exit_code offset mismatch");
    assert!(offset_of!(ProcMeta, wait_for_pid)        == 144, "wait_for_pid offset mismatch");
    assert!(offset_of!(ProcMeta, brk_start)           == 148, "brk_start offset mismatch");
    assert!(offset_of!(ProcMeta, brk_current)         == 152, "brk_current offset mismatch");
    assert!(offset_of!(ProcMeta, sleep_until)         == 156, "sleep_until offset mismatch");
    assert!(offset_of!(ProcMeta, cwd)                 == 160, "cwd offset mismatch");
    assert!(offset_of!(ProcMeta, uid)                 == 416, "uid offset mismatch");
    assert!(offset_of!(ProcMeta, gid)                 == 420, "gid offset mismatch");
    assert!(offset_of!(ProcMeta, euid)                == 424, "euid offset mismatch");
    assert!(offset_of!(ProcMeta, egid)                == 428, "egid offset mismatch");
    assert!(offset_of!(ProcMeta, shm_attachments)     == 432, "shm_attachments offset mismatch");
    assert!(offset_of!(ProcMeta, wait_next)           == 560, "wait_next offset mismatch");
    assert!(offset_of!(ProcMeta, pgid)                == 564, "pgid offset mismatch");
    assert!(offset_of!(ProcMeta, sid)                 == 568, "sid offset mismatch");
    assert!(offset_of!(ProcMeta, umask)               == 572, "umask offset mismatch");
    assert!(offset_of!(ProcMeta, root)                == 576, "root offset mismatch");
    assert!(offset_of!(ProcMeta, exec_base)            == 580, "exec_base offset mismatch");
    assert!(offset_of!(ProcMeta, exec_symtab)          == 584, "exec_symtab offset mismatch");
    assert!(offset_of!(ProcMeta, exec_strtab)          == 588, "exec_strtab offset mismatch");
    assert!(offset_of!(ProcMeta, exec_symtab_count)    == 592, "exec_symtab_count offset mismatch");
    assert!(core::mem::size_of::<ProcMeta>() == 596, "ProcMeta size mismatch");
};
