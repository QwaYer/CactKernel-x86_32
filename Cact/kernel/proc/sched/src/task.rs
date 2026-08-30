//! Task subsystem hub: global state, task list management, shared helpers,
//! and re-exports of the lifecycle submodules (`create`, `exec`, `fork`,
//! `signal`, `sigreturn`).

use core::ffi::c_void;
use core::ptr;
use crate::ffi::{self, MmapTable, PAGE_PRESENT, PAGE_RW, PAGE_SIZE, PAGE_USER};
use crate::mlfq;
use crate::sync::irq_spinlock_t;
use crate::timer_wheel;

pub const MAX_FD: usize = 256;
pub use cact_sync::task_abi::{NSIG, TASK_SHM_MAX, ProcMeta};

pub const USER_CODE_SEL: u32 = 0x1B;
pub const USER_DATA_SEL: u32 = 0x23;
pub const KERNEL_CODE_SEL: u32 = 0x08;
pub const KERNEL_DATA_SEL: u32 = 0x10;

pub const SIGKILL:  u32 = 1 << 0;
pub const SIGTERM:  u32 = 1 << 1;
pub const SIGSTOP:  u32 = 1 << 2;
pub const SIGCONT:  u32 = 1 << 3;
pub const SIGPIPE:  u32 = 1 << 4;
pub const SIGALRM:  u32 = 1 << 5;
pub const SIGCHLD:  u32 = 1 << 6;
pub const SIGFPE:   u32 = 1 << 7;
pub const SIGSEGV:  u32 = 1 << 8;
pub const SIGWINCH: u32 = 1 << 9;
pub const SIGHUP:   u32 = 1 << 10;
pub const SIGINT:   u32 = 1 << 11;
pub const SIGQUIT:  u32 = 1 << 12;
pub const SIG_UNCATCHABLE: u32 = SIGKILL | SIGSTOP;

pub const SIG_DFL: u32 = 0;
pub const SIG_IGN: u32 = 1;

pub const KERNEL_STACK_SIZE: usize = 4096;
pub const KERNEL_BASE: u32 = 0xC000_0000;

pub const EXEC_MAX_ARGS:   usize = 256;
pub const EXEC_MAX_ENVS:   usize = 256;
pub const EXEC_MAX_STRLEN: usize = 4096;
pub(crate) const TRACE_PROC_LOGS: bool = false;

pub const USER_STACK_PAGES: u32 = 4;
pub const USER_STACK_BYTES: u32 = USER_STACK_PAGES * PAGE_SIZE;

pub use cact_sync::task_abi::{TaskShmAttach, TaskState, TaskStruct};

pub use crate::ffi::VfsNode;

#[no_mangle]
pub static mut current_task: *mut TaskStruct = ptr::null_mut();

#[no_mangle]
pub static mut task_list_head: *mut TaskStruct = ptr::null_mut();

#[no_mangle]
pub static mut next_pid: u32 = 1;

#[export_name = "scheduler_lock"]
pub static mut SCHEDULER_LOCK: irq_spinlock_t = irq_spinlock_t::new();

static mut task_list_tail: *mut TaskStruct = ptr::null_mut();

pub fn task_list_add(t: *mut TaskStruct) {
    if t.is_null() {
        return;
    }
    unsafe {
        (*t).next = ptr::null_mut();
        if task_list_tail.is_null() {
            task_list_head = t;
            task_list_tail = t;
        } else {
            (*task_list_tail).next = t;
            task_list_tail = t;
        }
    }
}

pub fn task_list_remove(t: *mut TaskStruct) {
    if t.is_null() {
        return;
    }
    unsafe {
        if task_list_head.is_null() {
            return;
        }

        let mut prev: *mut TaskStruct = ptr::null_mut();
        let mut cur = task_list_head;

        while !cur.is_null() {
            if cur == t {
                if prev.is_null() {
                    task_list_head = (*t).next;
                } else {
                    (*prev).next = (*t).next;
                }
                if task_list_tail == t {
                    task_list_tail = prev;
                }
                (*t).next = ptr::null_mut();
                return;
            }
            prev = cur;
            cur  = (*cur).next;
        }
    }
}

pub fn find_task_by_pid(pid: u32) -> *mut TaskStruct {
    unsafe {
        let mut cur = task_list_head;
        while !cur.is_null() {
            if (*cur).pid == pid {
                return cur;
            }
            cur = (*cur).next;
        }
        ptr::null_mut()
    }
}

pub(crate) fn ustack_phys_by_idx(p: &ProcMeta, idx: usize) -> *mut c_void {
    if idx == 0 {
        p.ustack_phys
    } else {
        p.ustack_phys_extra[idx - 1]
    }
}

pub(crate) fn ustack_kernel_byte_mut(p: &ProcMeta, uva: u32) -> *mut u8 {
    let base = p.ustack_virt;
    let off = uva.wrapping_sub(base) as usize;
    debug_assert!(off < USER_STACK_BYTES as usize);
    let pi = off / PAGE_SIZE as usize;
    let po = off % PAGE_SIZE as usize;
    unsafe { ustack_phys_by_idx(p, pi).cast::<u8>().add(po) }
}

pub(crate) fn ustack_write_u32(p: &ProcMeta, uva: u32, val: u32) {
    unsafe {
        *(ustack_kernel_byte_mut(p, uva) as *mut u32) = val;
    }
}

pub(crate) fn map_user_stack_in_pd(pd: *mut u32, p: &ProcMeta) {
    if pd.is_null() {
        return;
    }
    unsafe {
        for i in 0..USER_STACK_PAGES {
            let vaddr = p.ustack_virt.wrapping_add(i.wrapping_mul(PAGE_SIZE));
            let phys = ustack_phys_by_idx(p, i as usize) as u32;
            ffi::vmm_map(pd, vaddr, phys, PAGE_USER | PAGE_RW | PAGE_PRESENT);
        }
    }
}

pub(crate) fn free_user_stack_pages(p: &mut ProcMeta) {
    unsafe {
        for i in 0..USER_STACK_PAGES as usize {
            let pn = ustack_phys_by_idx(p, i);
            if !pn.is_null() {
                ffi::free_page(pn);
            }
        }
    }
    p.ustack_phys = ptr::null_mut();
    p.ustack_phys_extra = [ptr::null_mut(); 3];
}

pub(crate) fn task_zero_init(t: *mut TaskStruct, p: *mut ProcMeta) -> bool {
    if t.is_null() || p.is_null() {
        return false;
    }
    unsafe {
        ffi::memory_set(t as *mut c_void, 0, core::mem::size_of::<TaskStruct>());
        ffi::memory_set(p as *mut c_void, 0, core::mem::size_of::<ProcMeta>());

        let fds = ffi::kmalloc(core::mem::size_of::<ffi::TaskFdTable>()) as *mut ffi::TaskFdTable;
        if fds.is_null() {
            return false;
        }
        ffi::memory_set(fds as *mut c_void, 0, core::mem::size_of::<ffi::TaskFdTable>());
        (*p).fds = fds;

        let mmap_tbl = ffi::kmalloc(core::mem::size_of::<MmapTable>()) as *mut MmapTable;
        if mmap_tbl.is_null() {
            ffi::kfree(fds as *mut c_void);
            (*p).fds = ptr::null_mut();
            return false;
        }
        ffi::mmap_table_init(mmap_tbl);
        (*p).mmap_table = mmap_tbl;

        (*t).state      = TaskState::Ready;
        (*t).priority   = mlfq::MLFQ_LEVEL_INTERACTIVE;
        (*t).time_slice = mlfq::MLFQ_QUANTUM[mlfq::MLFQ_LEVEL_INTERACTIVE as usize];
        (*t).proc       = p;
        (*p).cwd[0]     = b'/';
        for i in 0..NSIG {
            (*p).signal_handlers[i] = SIG_DFL;
        }
        true
    }
}

#[no_mangle]
pub unsafe extern "C" fn task_init() {
    current_task    = ptr::null_mut();
    task_list_head  = ptr::null_mut();
    task_list_tail  = ptr::null_mut();
    next_pid        = 1;

    crate::sync::irq_spinlock_init(&raw mut SCHEDULER_LOCK);
    mlfq::mlfq_init();
    timer_wheel::timer_wheel_global_init();
    ffi::printk(b"\x01\x36Task subsystem initialized (MLFQ, timer wheel, scheduler lock)\n\0".as_ptr());
}

#[no_mangle]
pub unsafe extern "C" fn init_scheduler() -> i32 {
    let idle = ffi::kmalloc(core::mem::size_of::<TaskStruct>()) as *mut TaskStruct;
    if idle.is_null() {
        ffi::printk(b"\x01\x33cannot allocate idle task\n\0".as_ptr());
        return -1;
    }
    ffi::memory_set(idle as *mut c_void, 0, core::mem::size_of::<TaskStruct>());

    (*idle).pid           = 0;
    (*idle).state         = TaskState::Running;
    (*idle).is_kernel     = 1;
    (*idle).page_directory = ptr::null_mut();
    (*idle).proc          = ptr::null_mut();
    (*idle).next          = idle;
    (*idle).priority      = mlfq::MLFQ_LEVEL_BACKGROUND;
    (*idle).ticks_used    = 0;

    current_task    = idle;
    task_list_head  = idle;
    task_list_tail  = idle;

    ffi::printk(b"\x01\x36Scheduler initialized (idle task pid 0, circular run queue)\n\0".as_ptr());
    0
}

pub(crate) fn calc_highest_mapped_va(pd: *mut u32) -> u32 {
    if pd.is_null() {
        return 0;
    }
    unsafe {
        for pdi in (0..1024).rev() {
            let pde = *pd.add(pdi);
            if pde & PAGE_PRESENT == 0 {
                continue;
            }
            let pt = (pde & !0xFFF) as *mut u32;
            for pti in (0..1024).rev() {
                if *pt.add(pti) & PAGE_PRESENT != 0 {
                    let va = ((pdi << 22) | (pti << 12)) as u32;
                    if va < 0xBF00_0000 {
                        return va + PAGE_SIZE;
                    }
                }
            }
        }
        0
    }
}

pub(crate) fn push_empty_args(p: &ProcMeta, sp: &mut u32) {
    *sp -= 4;
    ustack_write_u32(p, *sp, 0);
    let envp_vaddr = *sp;

    *sp -= 4;
    ustack_write_u32(p, *sp, 0);
    let argv_vaddr = *sp;

    *sp -= 4;
    ustack_write_u32(p, *sp, envp_vaddr);
    *sp -= 4;
    ustack_write_u32(p, *sp, argv_vaddr);
    *sp -= 4;
    ustack_write_u32(p, *sp, 0);
}

#[no_mangle]
pub unsafe extern "C" fn task_set_state(
    t:         *mut TaskStruct,
    _old_state: u32,
    new_state:  u32,
) {
    if t.is_null() { return; }
    let ns = match new_state {
        0 => TaskState::Ready,
        1 => TaskState::Running,
        2 => TaskState::Sleeping,
        3 => TaskState::Zombie,
        4 => TaskState::Waiting,
        _ => return,
    };
    (*t).state = ns;
    match ns {
        TaskState::Ready => mlfq::mlfq_enqueue_locked(t, (*t).priority),
        _ => {}
    }
}

#[path = "task_create.rs"]
mod task_create;
pub use task_create::*;
#[path = "task_exec.rs"]
mod task_exec;
pub use task_exec::*;
#[path = "task_fork.rs"]
mod task_fork;
pub use task_fork::*;
#[path = "task_signal.rs"]
mod task_signal;
pub use task_signal::*;
#[path = "task_sigreturn.rs"]
mod task_sigreturn;
pub use task_sigreturn::*;
