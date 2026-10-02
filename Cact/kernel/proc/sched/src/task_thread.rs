//! User thread creation (`CACT_PROCCTL_THREAD_CREATE`).
//!
//! A thread is an ordinary `TaskStruct`, but it shares its thread group's
//! address space, open-file table and mmap table with the task that created it
//! and carries its own kernel stack, user stack (allocated by libc) and signal
//! state.  The group's shared state (`ProcShared`) is refcounted, so the
//! address space is torn down once, when the last member is reaped.

use core::ffi::c_void;
use core::ptr;
use crate::ffi;
use crate::mlfq;
use crate::sync::{irq_spinlock_acquire, irq_spinlock_release};
use crate::task::{
    current_task, kstack_alloc, next_pid, proc_shared_ref, task_list_add,
    ProcMeta, TaskStruct, TaskState, KERNEL_STACK_SIZE, SCHEDULER_LOCK, USER_CODE_SEL,
    USER_DATA_SEL,
};

/// Create a thread in the calling task's thread group.
///
/// `entry` is the user entry point (ring-3) and `user_esp` the user stack
/// pointer it starts on — libc lays out the stack with the thread-exit return
/// address and the thread argument.  `tls` is reserved for a future per-thread
/// segment base and is currently unused.
///
/// Returns the new thread's pid (> 0) or a negative error.
///
/// # Safety
///
/// Must be called from a task context with `SCHEDULER_LOCK` free; `entry` must
/// point into the caller's user address space.
#[no_mangle]
pub unsafe extern "C" fn sched_create_thread(
    entry:    *const c_void,
    user_esp: u32,
    tls:      u32,
    set_tid:  u32,
    clr_tid:  u32,
) -> i32 {
    let _ = tls;

    // SAFETY: `SCHEDULER_LOCK` is the scheduler's global spinlock; the caller must not hold it.
    unsafe { irq_spinlock_acquire(&raw mut SCHEDULER_LOCK) };

    let parent_raw = current_task();
    if parent_raw.is_null() {
        // SAFETY: the lock was acquired above.
        unsafe { irq_spinlock_release(&raw mut SCHEDULER_LOCK) };
        return -1;
    }
    // SAFETY: `parent_raw` is the live current task (non-null checked above); it is only read
    // below.
    let parent = unsafe { &*parent_raw };
    let parent_proc = parent.proc;
    if parent_proc.is_null() {
        // SAFETY: the lock was acquired above.
        unsafe { irq_spinlock_release(&raw mut SCHEDULER_LOCK) };
        return -2;
    }
    // SAFETY: `parent_proc` is the live current task's `ProcMeta`.
    let parent_shared = unsafe { (*parent_proc).shared };
    if parent_shared.is_null() || parent.page_directory.is_null() {
        // A ring-0 task or one without an address space cannot host threads.
        // SAFETY: the lock was acquired above.
        unsafe { irq_spinlock_release(&raw mut SCHEDULER_LOCK) };
        return -3;
    }

    let child_raw = cact_mm::kmalloc(core::mem::size_of::<TaskStruct>() as u32) as *mut TaskStruct;
    if child_raw.is_null() {
        // SAFETY: the lock was acquired above.
        unsafe { irq_spinlock_release(&raw mut SCHEDULER_LOCK) };
        return -4;
    }
    let child_p_raw = cact_mm::kmalloc(core::mem::size_of::<ProcMeta>() as u32) as *mut ProcMeta;
    if child_p_raw.is_null() {
        // SAFETY: `child_raw` is a live allocation owned here.
        unsafe { cact_mm::kfree((child_raw as *mut c_void) as *mut u8) };
        // SAFETY: the lock was acquired above.
        unsafe { irq_spinlock_release(&raw mut SCHEDULER_LOCK) };
        return -4;
    }
    // SAFETY: `kstack_alloc` has no preconditions and returns a live stack or null.
    let kstack = unsafe { kstack_alloc() };
    if kstack.is_null() {
        // SAFETY: `child_p_raw` and `child_raw` are live allocations owned here.
        unsafe { cact_mm::kfree((child_p_raw as *mut c_void) as *mut u8) };
        // SAFETY: `child_raw` is a live allocation owned here.
        unsafe { cact_mm::kfree((child_raw as *mut c_void) as *mut u8) };
        // SAFETY: the lock was acquired above.
        unsafe { irq_spinlock_release(&raw mut SCHEDULER_LOCK) };
        return -5;
    }

    // Inherit the parent's `ProcMeta` (signal dispositions, uid/gid, cwd...)
    // and then override everything that is per-thread.
    // SAFETY: both blocks are live and `ProcMeta`-sized; the copy stays inside both.
    unsafe {
        ffi::memory_copy(
            child_p_raw as *mut c_void,
            parent_proc as *const c_void,
            core::mem::size_of::<ProcMeta>(),
        )
    };
    // SAFETY: both blocks are live and `TaskStruct`-sized; the copy stays inside both.
    unsafe {
        ffi::memory_copy(
            child_raw as *mut c_void,
            parent_raw as *const c_void,
            core::mem::size_of::<TaskStruct>(),
        )
    };

    // SAFETY: `child_raw`/`child_p_raw` are fresh, exclusively owned copies made above.
    let child = unsafe { &mut *child_raw };
    // SAFETY: as above, for the `ProcMeta`.
    let child_p = unsafe { &mut *child_p_raw };

    // SAFETY: `next_pid` is a scheduler-owned global, read under `SCHEDULER_LOCK`.
    let pid = unsafe { next_pid };
    // SAFETY: `next_pid` is a scheduler-owned global, incremented under `SCHEDULER_LOCK`.
    unsafe { next_pid = pid + 1 };
    child.pid            = pid;
    child.state          = TaskState::Ready;
    child.page_directory = parent.page_directory;   // shared address space
    child.proc           = child_p_raw;
    child.next           = ptr::null_mut();
    child.queue_next     = ptr::null_mut();
    child.ticks_used     = 0;
    child.fpu_context_ptr = ptr::null_mut();        // own lazily-allocated FPU state

    child_p.stack_base        = kstack as *mut c_void;
    child_p.ustack_phys       = ptr::null_mut();
    child_p.ustack_virt       = 0;
    child_p.ustack_phys_extra = [ptr::null_mut(); 3];
    child_p.pending_signals   = 0;
    child_p.parent_pid        = 0;                  // joined via userspace futex, not waitpid
    child_p.exit_code         = 0;
    child_p.wait_for_pid      = 0;
    child_p.sleep_until       = 0;
    child_p.wait_next         = ptr::null_mut();
    child_p.alarm_ticks       = 0;
    child_p.itimer_value      = 0;
    child_p.itimer_interval   = 0;
    child_p.is_thread         = 1;
    child_p.clr_tid           = clr_tid;   // join futex the kernel zeroes on exit
    // SAFETY: `parent_proc` is the live parent's `ProcMeta`.
    child_p.tgid = unsafe { (*parent_proc).tgid };
    child_p.shared = parent_shared;
    // `fds`/`mmap_table` are shared by pointer; the copy from the parent already
    // holds the leader's objects, so nothing to change.

    // One more member of the group.
    // SAFETY: `parent_shared` is the live group state (non-null checked above) and the lock is
    // held, so the increment is serialized.
    unsafe { proc_shared_ref(parent_shared) };

    // First-entry kernel frame: exactly the user-task layout that
    // `user_task_trampoline` expects (pop ebx/esi/edi/ebp, ret, then iretd).
    let stack_top = kstack as usize + KERNEL_STACK_SIZE;
    // SAFETY: `kstack` is a live `KERNEL_STACK_SIZE`-byte stack, so the top 10 words lie inside.
    let frame = unsafe { core::slice::from_raw_parts_mut((stack_top - 10 * 4) as *mut u32, 10) };
    let tramp = ffi::user_task_trampoline as *const () as u32;
    frame[9] = USER_DATA_SEL;
    frame[8] = user_esp;
    frame[7] = 0x0000_0202;
    frame[6] = USER_CODE_SEL;
    frame[5] = entry as u32;
    frame[4] = tramp;
    frame[3] = 0;
    frame[2] = 0;
    frame[1] = 0;
    frame[0] = 0;

    child.esp = (stack_top - 10 * 4) as u32;

    // Publish the tid into the join/set words *before* the thread can be
    // scheduled (like CLONE_CHILD_SETTID): userspace then never writes them, so
    // the kernel's clear-on-exit cannot be raced by a late store.
    if set_tid != 0 {
        // SAFETY: `set_tid` was validated at thread creation to be a writable
        // `u32` in the caller's address space, which is `parent.page_directory`.
        unsafe { *(set_tid as *mut u32) = pid };
    }
    if clr_tid != 0 {
        // SAFETY: `clr_tid` was validated the same way.
        unsafe { *(clr_tid as *mut u32) = pid };
    }

    // The sigreturn trampoline is already mapped in the shared address space by
    // the group leader; the copied `sigreturn_trampoline` address inherits it,
    // so no remap is needed (and none would be safe — it allocates a fresh page).

    // SAFETY: `child` is live and exclusively owned here.
    unsafe { task_list_add(child as *mut TaskStruct) };
    // SAFETY: `child` is live and `SCHEDULER_LOCK` is held.
    unsafe { mlfq::mlfq_enqueue_locked(child as *mut TaskStruct, child.priority) };

    // SAFETY: the lock was acquired above.
    unsafe { irq_spinlock_release(&raw mut SCHEDULER_LOCK) };

    pid as i32
}

unsafe extern "C" {
    /// Provided by `syscall/process/thread.c`; wakes waiters on a futex word.
    fn cact_futex_wake_addr(pd: *mut u32, uaddr: *mut u32, count: i32) -> i32;
}

/// Clear the exiting thread's join word and wake its joiners (Linux's
/// `clear_child_tid`).  A no-op for a process leader.
///
/// Doing this in the kernel is what makes `pthread_join` safe: the joiner can
/// free the thread's stack as soon as it observes the zero, because the kernel
/// wrote it at a point where the thread will never touch its user stack again.
///
/// # Safety
///
/// `t` must be null or a live `TaskStruct`; must be called from the task's own
/// context, with `SCHEDULER_LOCK` free (this takes it via the futex wake).
pub unsafe fn clear_child_tid_on_exit(t: *mut TaskStruct) {
    if t.is_null() {
        return;
    }
    // SAFETY: `t` is live (non-null checked above), so its `proc` field is in bounds.
    let proc_ptr = unsafe { (*t).proc };
    if proc_ptr.is_null() {
        return;
    }
    // SAFETY: `proc_ptr` is that task's live `ProcMeta`.
    let is_thread = unsafe { (*proc_ptr).is_thread };
    if is_thread == 0 {
        return;
    }
    // SAFETY: `proc_ptr` is live.
    let ct = unsafe { (*proc_ptr).clr_tid };
    if ct == 0 {
        return;
    }
    // SAFETY: `proc_ptr` is live.
    unsafe { (*proc_ptr).clr_tid = 0 };
    let uaddr = ct as *mut u32;
    // SAFETY: `ct` was a writable user word in this address space (validated at
    // creation) and the address space is still alive here.
    unsafe { *uaddr = 0 };
    // SAFETY: `t` is live, so `page_directory` is this task's live address space.
    let pd = unsafe { (*t).page_directory };
    // SAFETY: `pd` is the live address space and `uaddr` a valid word in it; the
    // FFI function keys the wake on (pd, uaddr) and does not dereference `uaddr`.
    unsafe { cact_futex_wake_addr(pd, uaddr, i32::MAX) };
}
