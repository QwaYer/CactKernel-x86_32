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

// Kernel stacks are one contiguous allocation that grows down.  They must be
// bigger than a single page: syscall handoffs, the VFS, and now in-kernel
// crypto (the DRBG, and webpki's X.509 chain validation) all run on the
// caller's kernel stack, and 4 KiB was not enough room for that chain.
pub const KERNEL_STACK_SIZE: usize = 16384;
pub const KERNEL_BASE: u32 = 0xC000_0000;

/// Canary words at the base of every kernel stack.  A stack that runs past its
/// base silently overwrites whatever the allocator placed after it, so this is
/// the one place that can notice the overflow instead of the damage it does.
pub const KERNEL_STACK_CANARY: u32 = 0x5A5A_5A5A;

/// Allocate a kernel stack with the canary painted at its base.
///
/// # Safety
///
/// `kstack_alloc` itself has no preconditions; the returned pointer must be passed to
/// `kstack_free` exactly once and must not be used as a stack before it is initialised.
pub unsafe fn kstack_alloc() -> *mut u32 {
    let p = cact_mm::kmalloc(KERNEL_STACK_SIZE as u32) as *mut u32;
    if !p.is_null() {
        // SAFETY: `p` is a live `KERNEL_STACK_SIZE` block, so its first word is in bounds.
        unsafe { *p = KERNEL_STACK_CANARY };
        // SAFETY: `p` is valid for at least two `u32` words, so this address is in bounds.
        let second = unsafe { p.add(1) };
        // SAFETY: `second` is the second word of that live block.
        unsafe { *second = KERNEL_STACK_CANARY };
    }
    p
}

/// # Safety
///
/// `base` must be null or a pointer previously returned by `kstack_alloc`/`kmalloc` that has
/// not already been freed.
pub unsafe fn kstack_free(base: *mut c_void) {
    // SAFETY: `base` is null-checked and, per the contract, is a live allocation from
    // `kstack_alloc`, so `kfree` is called on a valid block.
    unsafe {
        if !base.is_null() {
            cact_mm::kfree((base) as *mut u8);
        }
    }
}

/// 1 while the stack's canary is intact.
///
/// # Safety
///
/// `base` must be null or point to a live kernel-stack allocation of at least two `u32` words.
pub unsafe fn kstack_ok(base: *mut c_void) -> bool {
    if base.is_null() {
        return true;
    }
    let p = base as *const u32;
    // SAFETY: `base` is non-null and points to a live kernel-stack allocation (see # Safety), so
    // the first canary word is in bounds.
    let first = unsafe { *p };
    // SAFETY: that allocation is at least two `u32` words, so this address is in bounds.
    let second_ptr = unsafe { p.add(1) };
    // SAFETY: `second_ptr` is the second word of the live allocation.
    let second = unsafe { *second_ptr };
    first == KERNEL_STACK_CANARY && second == KERNEL_STACK_CANARY
}

/// Report a kernel stack that has run past its base.  The damage is already
/// done by then — this is about naming it instead of letting it surface later
/// as a mystery somewhere else.  Reported once.
pub fn task_check_kernel_stack() {
    use core::sync::atomic::{AtomicBool, Ordering};
    static REPORTED: AtomicBool = AtomicBool::new(false);
    // SAFETY: `current_task` is a scheduler-owned global; when non-null it is the live task
    // running on this CPU.
    let t = unsafe { current_task };
    if t.is_null() || REPORTED.load(Ordering::Relaxed) {
        return;
    }
    // SAFETY: `t` is the live current task (non-null checked above).
    let p = unsafe { (*t).proc };
    if p.is_null() {
        return;
    }
    // SAFETY: `p` is the live `ProcMeta` of `t`.
    let stack_base = unsafe { (*p).stack_base };
    // SAFETY: `stack_base` is the live kernel-stack pointer of the current task.
    let ok = unsafe { kstack_ok(stack_base) };
    if !ok {
        REPORTED.store(true, Ordering::Relaxed);
        // SAFETY: `printk` takes a static NUL-terminated byte string.
        unsafe {
            ffi::printk(c"  sched       : KERNEL STACK OVERFLOW (canary destroyed)\n".as_ptr().cast())
        };
    }
}

pub const EXEC_MAX_ARGS:   usize = 256;
pub const EXEC_MAX_ENVS:   usize = 256;
pub const EXEC_MAX_STRLEN: usize = 4096;

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

/// Append `t` to the scheduler's global task list.
///
/// # Safety
///
/// `t` must be null or a live, exclusively owned `TaskStruct`; the caller must hold
/// `SCHEDULER_LOCK` (or be single-threaded), because the list is shared scheduler state.
pub unsafe fn task_list_add(t: *mut TaskStruct) {
    if t.is_null() {
        return;
    }
    // SAFETY: `t` is non-null and live (see # Safety), so clearing its link is in bounds.
    unsafe { (*t).next = ptr::null_mut() };
    // SAFETY: `task_list_tail` is a scheduler-owned global, read under the caller's
    // `SCHEDULER_LOCK` (see # Safety).
    let tail = unsafe { task_list_tail };
    if tail.is_null() {
        // SAFETY: `task_list_head` is a scheduler-owned global, mutated under the caller's lock.
        unsafe { task_list_head = t };
        // SAFETY: `task_list_tail` is a scheduler-owned global, mutated under the caller's lock.
        unsafe { task_list_tail = t };
    } else {
        // SAFETY: `tail` is the last live element of the list, so writing its link is in bounds.
        unsafe { (*tail).next = t };
        // SAFETY: `task_list_tail` is a scheduler-owned global, mutated under the caller's lock.
        unsafe { task_list_tail = t };
    }
}

/// Unlink `t` from the scheduler's global task list.
///
/// # Safety
///
/// `t` must be null or a live `TaskStruct` currently on the scheduler task list; the caller must
/// hold `SCHEDULER_LOCK` (or be single-threaded), because the list is shared scheduler state.
pub unsafe fn task_list_remove(t: *mut TaskStruct) {
    if t.is_null() {
        return;
    }
    // SAFETY: `task_list_head` is a scheduler-owned global, read under the caller's lock.
    if unsafe { task_list_head }.is_null() {
        return;
    }

    let mut prev: *mut TaskStruct = ptr::null_mut();
    // SAFETY: `task_list_head` is a scheduler-owned global, read under the caller's lock.
    let mut cur = unsafe { task_list_head };

    while !cur.is_null() {
        if cur == t {
            // SAFETY: `t` is non-null and live (see # Safety).
            let t_next = unsafe { (*t).next };
            if prev.is_null() {
                // SAFETY: `task_list_head` is a scheduler-owned global, mutated under the
                // caller's lock.
                unsafe { task_list_head = t_next };
            } else {
                // SAFETY: `prev` was reached by walking the list, so it is a live task.
                unsafe { (*prev).next = t_next };
            }
            // SAFETY: `task_list_tail` is a scheduler-owned global, read here.
            let tail = unsafe { task_list_tail };
            if tail == t {
                // SAFETY: mutated under the caller's lock.
                unsafe { task_list_tail = prev };
            }
            // SAFETY: `t` is live, so clearing its link is in bounds.
            unsafe { (*t).next = ptr::null_mut() };
            return;
        }
        prev = cur;
        // SAFETY: `cur` was reached by walking the list, so it is a live task.
        cur = unsafe { (*cur).next };
    }
}

pub fn find_task_by_pid(pid: u32) -> *mut TaskStruct {
    // SAFETY: `task_list_head` is a scheduler-owned global; all callers hold `SCHEDULER_LOCK`, so
    // the list is stable while it is walked here.
    let mut cur = unsafe { task_list_head };
    while !cur.is_null() {
        // SAFETY: `cur` is a live task on the list.
        let cur_pid = unsafe { (*cur).pid };
        if cur_pid == pid {
            return cur;
        }
        // SAFETY: `cur` is live, so its link is in bounds.
        cur = unsafe { (*cur).next };
    }
    ptr::null_mut()
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
    // SAFETY: `p` is a live `ProcMeta`; `pi`/`po` are the page/offset split of `uva`, and callers
    // pass a `uva` inside this process's mapped user-stack window, so the returned pointer lies
    // inside one of the stack's backing pages.
    unsafe { ustack_phys_by_idx(p, pi).cast::<u8>().add(po) }
}

pub(crate) fn ustack_write_u32(p: &ProcMeta, uva: u32, val: u32) {
    // SAFETY: `ustack_kernel_byte_mut` yields a pointer inside a user-stack backing page for the
    // caller-supplied in-window `uva`, and callers only pass word-aligned addresses (the stack
    // slots they decrement by 4), so the 4-byte store is aligned and in bounds.
    unsafe {
        *(ustack_kernel_byte_mut(p, uva) as *mut u32) = val;
    }
}

pub(crate) fn map_user_stack_in_pd(pd: *mut u32, p: &ProcMeta) {
    if pd.is_null() {
        return;
    }
    // SAFETY: `pd` is null-checked and, per the contract, a live page directory owned by the
    // task; `p` is a live `ProcMeta` whose `ustack_phys`/`ustack_phys_extra` hold this process's
    // stack pages, so each `vmm_map` is handed a real physical page.
    unsafe {
        for i in 0..USER_STACK_PAGES {
            let vaddr = p.ustack_virt.wrapping_add(i.wrapping_mul(PAGE_SIZE));
            let phys = ustack_phys_by_idx(p, i as usize) as u32;
            cact_mm::vmm_map(pd, vaddr, phys, (PAGE_USER | PAGE_RW | PAGE_PRESENT) as i32);
        }
    }
}

pub(crate) fn free_user_stack_pages(p: &mut ProcMeta) {
    for i in 0..USER_STACK_PAGES as usize {
        let pn = ustack_phys_by_idx(p, i);
        if !pn.is_null() {
            // SAFETY: the frame is live and PMM-managed; this call releases exactly one reference to it.
            unsafe { cact_mm::free_page(pn as *mut u8) };
        }
    }
    p.ustack_phys = ptr::null_mut();
    p.ustack_phys_extra = [ptr::null_mut(); 3];
}

pub(crate) fn task_zero_init(t: *mut TaskStruct, p: *mut ProcMeta) -> bool {
    if t.is_null() || p.is_null() {
        return false;
    }
    // SAFETY: `t` is a freshly allocated `TaskStruct` block, so zeroing its whole extent is in
    // bounds.
    unsafe { ffi::memory_set(t as *mut c_void, 0, core::mem::size_of::<TaskStruct>()) };
    // SAFETY: `p` is a freshly allocated `ProcMeta` block, so zeroing its whole extent is in
    // bounds.
    unsafe { ffi::memory_set(p as *mut c_void, 0, core::mem::size_of::<ProcMeta>()) };

    // SAFETY: `t` is a fresh, exclusively owned `TaskStruct`, now zeroed.
    let t = unsafe { &mut *t };
    // SAFETY: `p` is a fresh, exclusively owned `ProcMeta`, now zeroed.
    let p = unsafe { &mut *p };

    let fds = cact_mm::kmalloc(core::mem::size_of::<ffi::TaskFdTable>() as u32) as *mut ffi::TaskFdTable;
    if fds.is_null() {
        return false;
    }
    // SAFETY: `fds` is a fresh block of exactly `TaskFdTable`'s size; zeroing it is in bounds.
    unsafe { ffi::memory_set(fds as *mut c_void, 0, core::mem::size_of::<ffi::TaskFdTable>()) };
    p.fds = fds;

    let mmap_tbl = cact_mm::kmalloc(core::mem::size_of::<MmapTable>() as u32) as *mut MmapTable;
    if mmap_tbl.is_null() {
        // SAFETY: `fds` is a live allocation owned here, so releasing it is sound.
        unsafe { cact_mm::kfree((fds as *mut c_void) as *mut u8) };
        p.fds = ptr::null_mut();
        return false;
    }
    // SAFETY: `mmap_tbl` is a fresh `MmapTable`-sized block.
    unsafe { cact_mm::mmap_table_init(mmap_tbl) };
    p.mmap_table = mmap_tbl;

    t.state      = TaskState::Ready;
    t.priority   = mlfq::MLFQ_LEVEL_INTERACTIVE;
    t.time_slice = mlfq::MLFQ_QUANTUM[mlfq::MLFQ_LEVEL_INTERACTIVE as usize];
    t.proc       = p as *mut ProcMeta;
    p.cwd[0]     = b'/';
    for slot in p.signal_handlers.iter_mut() {
        *slot = SIG_DFL;
    }
    true
}

/// # Safety
///
/// Must be called exactly once during single-threaded boot, before any other code touches the
/// scheduler globals or `SCHEDULER_LOCK`.
#[no_mangle]
pub unsafe extern "C" fn task_init() {
    // SAFETY: single-threaded boot entry: these are the scheduler's own globals, written before
    // any other CPU or interrupt can observe them (see # Safety).
    unsafe { current_task = ptr::null_mut() };
    // SAFETY: as above.
    unsafe { task_list_head = ptr::null_mut() };
    // SAFETY: as above.
    unsafe { task_list_tail = ptr::null_mut() };
    // SAFETY: as above.
    unsafe { next_pid = 1 };

    // SAFETY: initialises the scheduler's own spinlock before it is ever used.
    unsafe { crate::sync::irq_spinlock_init(&raw mut SCHEDULER_LOCK) };
    mlfq::mlfq_init();
    timer_wheel::timer_wheel_global_init();
    // SAFETY: `printk` takes a static NUL-terminated byte string.
    unsafe { ffi::printk(c"\x01\x36  sched       : MLFQ, timer wheel, scheduler lock\n".as_ptr().cast()) };
}

/// # Safety
///
/// Must be called once from single-threaded boot after `task_init`, with interrupts disabled.
#[no_mangle]
pub unsafe extern "C" fn init_scheduler() -> i32 {
    let idle = cact_mm::kmalloc(core::mem::size_of::<TaskStruct>() as u32) as *mut TaskStruct;
    if idle.is_null() {
        // SAFETY: `printk` takes a static NUL-terminated byte string.
        unsafe { ffi::printk(c"\x01\x33  sched       : cannot allocate idle task\n".as_ptr().cast()) };
        return -1;
    }
    // SAFETY: `idle` is a fresh allocation of exactly `TaskStruct`'s size; zeroing it is in
    // bounds.
    unsafe { ffi::memory_set(idle as *mut c_void, 0, core::mem::size_of::<TaskStruct>()) };

    // SAFETY: `idle` is a fresh, exclusively owned `TaskStruct`, now zeroed.
    let idle_t = unsafe { &mut *idle };
    idle_t.pid            = 0;
    idle_t.state          = TaskState::Running;
    idle_t.is_kernel      = 1;
    idle_t.page_directory = ptr::null_mut();
    idle_t.proc           = ptr::null_mut();
    idle_t.next           = idle;
    idle_t.priority       = mlfq::MLFQ_LEVEL_BACKGROUND;
    idle_t.ticks_used     = 0;

    // SAFETY: boot-time writes to the scheduler globals, before interrupts are enabled.
    unsafe { current_task = idle };
    // SAFETY: as above.
    unsafe { task_list_head = idle };
    // SAFETY: as above.
    unsafe { task_list_tail = idle };

    // SAFETY: `printk` takes a static NUL-terminated byte string.
    unsafe { ffi::printk(c"\x01\x36  sched       : idle task pid 0, circular run queue\n".as_ptr().cast()) };
    0
}

pub(crate) fn calc_highest_mapped_va(pd: *mut u32) -> u32 {
    if pd.is_null() {
        return 0;
    }
    for pdi in (0..1024).rev() {
        // SAFETY: `pd` is a live page directory and `pdi < 1024`, so this PDE offset is in
        // bounds.
        let pde_ptr = unsafe { pd.add(pdi) };
        // SAFETY: `pde_ptr` is a live PDE slot.
        let pde = unsafe { *pde_ptr };
        if pde & PAGE_PRESENT == 0 {
            continue;
        }
        let pt = (pde & !0xFFF) as *mut u32;
        for pti in (0..1024).rev() {
            // SAFETY: `pt` is a live page table and `pti < 1024`, so this PTE offset is in
            // bounds.
            let pte_ptr = unsafe { pt.add(pti) };
            // SAFETY: `pte_ptr` is a live PTE slot.
            let pte = unsafe { *pte_ptr };
            if pte & PAGE_PRESENT != 0 {
                let va = ((pdi << 22) | (pti << 12)) as u32;
                if va < 0xBF00_0000 {
                    return va + PAGE_SIZE;
                }
            }
        }
    }
    0
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

/// # Safety
///
/// `t` must be null or a live `TaskStruct`, and the caller must hold `SCHEDULER_LOCK` (or be
/// single-threaded).
#[no_mangle]
pub unsafe extern "C" fn task_set_state(
    t:         *mut TaskStruct,
    _old_state: u32,
    new_state:  u32,
) {
    if t.is_null() {
        return;
    }
    let ns = match new_state {
        0 => TaskState::Ready,
        1 => TaskState::Running,
        2 => TaskState::Sleeping,
        3 => TaskState::Zombie,
        4 => TaskState::Waiting,
        _ => return,
    };
    // SAFETY: `t` is non-null and, per the contract, a live task, so the field write is in
    // bounds.
    unsafe { (*t).state = ns };
    if ns == TaskState::Ready {
        // SAFETY: `t` is live.
        let pri = unsafe { (*t).priority };
        // SAFETY: `t` is live; the caller holds `SCHEDULER_LOCK` (see # Safety).
        unsafe { mlfq::mlfq_enqueue_locked(t, pri) };
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
