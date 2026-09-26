//! Process forking (`task_fork`), exit/wait machinery, and zombie reaping.

use cact_mm::vmm_create_address_space;
use core::ffi::c_void;
use core::ptr;
use crate::ffi::{self, ContextFrame, MmapTable, VfsNode, PAGE_PRESENT, PAGE_RW, PAGE_SIZE, PAGE_USER};
use crate::mlfq;
use crate::sync::{irq_spinlock_acquire, irq_spinlock_release};
use crate::task::{
    kstack_alloc, kstack_free, current_task, find_task_by_pid, next_pid, task_list_add, task_list_head,
    task_list_remove, task_setup_sigreturn, ustack_phys_by_idx, ProcMeta, TaskStruct,
    TaskState, KERNEL_STACK_SIZE, MAX_FD, SCHEDULER_LOCK,
    USER_STACK_PAGES,
};

/// # Safety
///
/// `regs` must point to a valid `ContextFrame` captured from the forking task's kernel stack,
/// and the caller must not already hold `SCHEDULER_LOCK`.
#[no_mangle]
pub unsafe extern "C" fn task_fork(regs: *mut ContextFrame) -> *mut TaskStruct {
    // SAFETY: `regs` points to the live interrupt frame (see # Safety); it is only read below.
    let regs = unsafe { &*regs };

    // SAFETY: `SCHEDULER_LOCK` is the scheduler's global spinlock; the caller must not already
    // hold it (see # Safety).
    unsafe { irq_spinlock_acquire(&raw mut SCHEDULER_LOCK) };

    // SAFETY: `current_task` is a scheduler-owned global, read under `SCHEDULER_LOCK`.
    let parent_raw = unsafe { current_task };
    if parent_raw.is_null() {
        // SAFETY: the lock was acquired above.
        unsafe { irq_spinlock_release(&raw mut SCHEDULER_LOCK) };
        return ptr::null_mut();
    }
    // SAFETY: `parent_raw` is the live forking task (non-null checked above); it is only read
    // below, so a shared reborrow is sound.
    let parent = unsafe { &*parent_raw };

    let child_pd = vmm_create_address_space();
    if child_pd.is_null() {
        // SAFETY: the lock was acquired above.
        unsafe { irq_spinlock_release(&raw mut SCHEDULER_LOCK) };
        return ptr::null_mut();
    }

    let child_raw = cact_mm::kmalloc(core::mem::size_of::<TaskStruct>() as u32) as *mut TaskStruct;
    if child_raw.is_null() {
        // SAFETY: `child_pd` is a live address space owned here.
        unsafe { cact_mm::vmm_free_address_space(child_pd) };
        // SAFETY: the lock was acquired above.
        unsafe { irq_spinlock_release(&raw mut SCHEDULER_LOCK) };
        return ptr::null_mut();
    }

    let child_p_raw = cact_mm::kmalloc(core::mem::size_of::<ProcMeta>() as u32) as *mut ProcMeta;
    if child_p_raw.is_null() {
        // SAFETY: `child_raw` is a live allocation owned here.
        unsafe { cact_mm::kfree((child_raw as *mut c_void) as *mut u8) };
        // SAFETY: `child_pd` is a live address space owned here.
        unsafe { cact_mm::vmm_free_address_space(child_pd) };
        // SAFETY: the lock was acquired above.
        unsafe { irq_spinlock_release(&raw mut SCHEDULER_LOCK) };
        return ptr::null_mut();
    }

    // SAFETY: `kstack_alloc` has no preconditions and hands back a live stack or null.
    let kstack = unsafe { kstack_alloc() };
    if kstack.is_null() {
        // SAFETY: `child_p_raw` is a live allocation owned here.
        unsafe { cact_mm::kfree((child_p_raw as *mut c_void) as *mut u8) };
        // SAFETY: `child_raw` is a live allocation owned here.
        unsafe { cact_mm::kfree((child_raw as *mut c_void) as *mut u8) };
        // SAFETY: `child_pd` is a live address space owned here.
        unsafe { cact_mm::vmm_free_address_space(child_pd) };
        // SAFETY: the lock was acquired above.
        unsafe { irq_spinlock_release(&raw mut SCHEDULER_LOCK) };
        return ptr::null_mut();
    }

    let mut ustack_pages: [*mut c_void; USER_STACK_PAGES as usize] =
        [ptr::null_mut(); USER_STACK_PAGES as usize];
    for i in 0..USER_STACK_PAGES as usize {
        let page = cact_mm::kalloc();
        if page.is_null() {
            for page in &ustack_pages[..i] {
                // SAFETY: each entry is a page allocated by `kalloc` above.
                unsafe { cact_mm::free_page((*page) as *mut u8) };
            }
            // SAFETY: `kstack` is a live kernel stack owned here.
            unsafe { kstack_free(kstack as *mut c_void) };
            // SAFETY: `child_p_raw` is a live allocation owned here.
            unsafe { cact_mm::kfree((child_p_raw as *mut c_void) as *mut u8) };
            // SAFETY: `child_raw` is a live allocation owned here.
            unsafe { cact_mm::kfree((child_raw as *mut c_void) as *mut u8) };
            // SAFETY: `child_pd` is a live address space owned here.
            unsafe { cact_mm::vmm_free_address_space(child_pd) };
            // SAFETY: the lock was acquired above.
            unsafe { irq_spinlock_release(&raw mut SCHEDULER_LOCK) };
            return ptr::null_mut();
        }
        ustack_pages[i] = page as *mut c_void;
    }

    // SAFETY: `child_raw` is a live `TaskStruct` owned here and `parent` is the live parent;
    // the copy stays inside both allocations.
    unsafe {
        ffi::memory_copy(
            child_raw as *mut c_void,
            parent as *const TaskStruct as *const c_void,
            core::mem::size_of::<TaskStruct>(),
        )
    };

    let parent_p = parent.proc;
    // SAFETY: `parent_p` is the live parent's `ProcMeta`; it is only read throughout this
    // function.
    let parent_p_ref = unsafe { &*parent_p };
    // SAFETY: `child_p_raw` is a live `ProcMeta` owned here and `parent_p` is the live parent's
    // `ProcMeta`; the copy stays inside both allocations.
    unsafe {
        ffi::memory_copy(
            child_p_raw as *mut c_void,
            parent_p as *const c_void,
            core::mem::size_of::<ProcMeta>(),
        )
    };

    // SAFETY: `child_raw` is a fresh, exclusively owned `TaskStruct`, zeroed just above by the
    // parent copy.
    let child = unsafe { &mut *child_raw };
    // SAFETY: `child_p_raw` is a fresh, exclusively owned `ProcMeta`, zeroed just above.
    let child_p = unsafe { &mut *child_p_raw };

    // SAFETY: `next_pid` is a scheduler-owned global, read under `SCHEDULER_LOCK`.
    let pid = unsafe { next_pid };
    child.pid = pid;
    // SAFETY: `next_pid` is a scheduler-owned global, incremented under `SCHEDULER_LOCK`.
    unsafe { next_pid = pid + 1 };
    child.state          = TaskState::Ready;
    child.page_directory = child_pd;
    child.proc           = child_p_raw;
    child.next           = ptr::null_mut();
    child.queue_next     = ptr::null_mut();

    child_p.stack_base        = kstack as *mut c_void;
    child_p.ustack_phys       = ustack_pages[0];
    child_p.ustack_phys_extra = [ustack_pages[1], ustack_pages[2], ustack_pages[3]];
    child_p.parent_pid        = parent.pid;
    child_p.exit_code         = 0;
    child_p.wait_for_pid      = 0;
    child_p.sleep_until       = 0;
    child_p.pending_signals   = 0;
    child_p.wait_next         = ptr::null_mut();

    // SAFETY: the pointer is derived from `child_p`, so the tracker reset is in bounds.
    unsafe { ffi::proc_tracker_init(core::ptr::addr_of_mut!(child_p.mm)) };
    child_p.mm.page_dir = child_pd;

    let child_fds = cact_mm::kmalloc(core::mem::size_of::<ffi::TaskFdTable>() as u32) as *mut ffi::TaskFdTable;
    if child_fds.is_null() {
        for page in ustack_pages {
            // SAFETY: each entry is a page allocated by `kalloc` above.
            unsafe { cact_mm::free_page((page) as *mut u8) };
        }
        // SAFETY: `kstack` is a live kernel stack owned here.
        unsafe { kstack_free(kstack as *mut c_void) };
        // SAFETY: `child_p` is a live allocation owned here.
        unsafe { cact_mm::kfree((child_p as *mut ProcMeta as *mut c_void) as *mut u8) };
        // SAFETY: `child` is a live allocation owned here.
        unsafe { cact_mm::kfree((child as *mut TaskStruct as *mut c_void) as *mut u8) };
        // SAFETY: `child_pd` is a live address space owned here.
        unsafe { cact_mm::vmm_free_address_space(child_pd) };
        // SAFETY: the lock was acquired above.
        unsafe { irq_spinlock_release(&raw mut SCHEDULER_LOCK) };
        return ptr::null_mut();
    }
    let parent_fds = parent_p_ref.fds;
    // SAFETY: `child_fds` and `parent_fds` are live `TaskFdTable`s, so the copy stays inside both.
    unsafe {
        ffi::memory_copy(
            child_fds as *mut c_void,
            parent_fds as *const c_void,
            core::mem::size_of::<ffi::TaskFdTable>(),
        )
    };

    for i in 0..MAX_FD {
        // SAFETY: `child_fds` is a live `TaskFdTable` and `i < MAX_FD`.
        let ft = unsafe { (*child_fds).fd_table[i] };
        if !ft.is_null() {
            // SAFETY: `ft` is a live file object (a non-null entry of the copied table).
            unsafe { ffi::file_ref(ft as *mut c_void) };
        }
    }

    child_p.fds = child_fds;

    let child_mmap = cact_mm::kmalloc(core::mem::size_of::<MmapTable>() as u32) as *mut MmapTable;
    if child_mmap.is_null() {
        // SAFETY: `child_fds` is a live allocation owned here.
        unsafe { cact_mm::kfree((child_fds as *mut c_void) as *mut u8) };
        child_p.fds = ptr::null_mut();
        for page in ustack_pages {
            // SAFETY: each entry is a page allocated by `kalloc` above.
            unsafe { cact_mm::free_page((page) as *mut u8) };
        }
        // SAFETY: `kstack` is a live kernel stack owned here.
        unsafe { kstack_free(kstack as *mut c_void) };
        // SAFETY: `child_p` is a live allocation owned here.
        unsafe { cact_mm::kfree((child_p as *mut ProcMeta as *mut c_void) as *mut u8) };
        // SAFETY: `child` is a live allocation owned here.
        unsafe { cact_mm::kfree((child as *mut TaskStruct as *mut c_void) as *mut u8) };
        // SAFETY: `child_pd` is a live address space owned here.
        unsafe { cact_mm::vmm_free_address_space(child_pd) };
        // SAFETY: the lock was acquired above.
        unsafe { irq_spinlock_release(&raw mut SCHEDULER_LOCK) };
        return ptr::null_mut();
    }
    // SAFETY: `child_mmap` is a fresh `MmapTable`-sized block.
    unsafe { cact_mm::mmap_table_init(child_mmap) };
    child_p.mmap_table = child_mmap;

    if !parent.page_directory.is_null() {
        // SAFETY: `parent.page_directory` and `child_pd` are both live address spaces.
        unsafe { cact_mm::vmm_fork_address_space(parent.page_directory, child_pd) };
    }

    let parent_mmap_table = parent_p_ref.mmap_table;
    // SAFETY: both mmap tables are live and both page directories are valid; the clone reads the
    // parent's and writes the child's.
    unsafe {
        cact_mm::mmap_table_clone(
            parent_mmap_table,
            child_p.mmap_table,
            parent.page_directory,
            child_pd,
        )
    };

    // The kernel MMIO mappings (framebuffer, xHCI BARs...) must be present in
    // the child pd; fork only deep-copies the user half.
    // SAFETY: `child_pd` is a live address space.
    unsafe { cact_mm::vmm_sync_kernel_mmio_mappings(child_pd) };

    for i in 0..USER_STACK_PAGES as usize {
        let vaddr = child_p.ustack_virt.wrapping_add((i as u32).wrapping_mul(PAGE_SIZE));
        let cphys = ustack_phys_by_idx(child_p, i) as u32;
        // SAFETY: `child_pd` is a live address space, `vaddr` is in the child's user range and
        // `cphys` is a page owned by the child.
        unsafe { cact_mm::vmm_map(child_pd, vaddr, cphys, (PAGE_USER | PAGE_RW | PAGE_PRESENT) as i32) };
        // SAFETY: the child's and parent's stack pages are live and distinct, so the copy stays
        // inside both.
        unsafe {
            ffi::memory_copy(
                ustack_phys_by_idx(child_p, i),
                ustack_phys_by_idx(parent_p_ref, i) as *const c_void,
                PAGE_SIZE as usize,
            )
        };
    }

    for i in 0..MAX_FD {
        // SAFETY: `child_p.fds` is the live fd table installed just above and `i < MAX_FD`.
        let ft = unsafe { (*child_p.fds).fd_table[i] };
        if !ft.is_null() {
            // SAFETY: `ft` is a live file-table entry (a non-null slot of the copied table).
            let node = unsafe { *(ft as *const *mut VfsNode) };
            if !node.is_null() {
                // SAFETY: `node` is a live VFS node.
                unsafe { ffi::open_vfs(node) };
            }
        }
    }

    for attach in child_p.shm_attachments.iter_mut() {
        attach.shm_id    = 0;
        attach.shm_vaddr = 0;
    }

    let stack_top = kstack as usize + KERNEL_STACK_SIZE;
    // SAFETY: `kstack` is a live `KERNEL_STACK_SIZE`-byte kernel stack, so the top 20 words lie
    // inside it.
    let frame = unsafe { core::slice::from_raw_parts_mut((stack_top - 20 * 4) as *mut u32, 20) };
    let mut sp = 20usize;

    sp -= 1; frame[sp] = regs.ss;
    sp -= 1; frame[sp] = regs.useresp;
    sp -= 1; frame[sp] = regs.eflags;
    sp -= 1; frame[sp] = regs.cs;
    sp -= 1; frame[sp] = regs.eip;
    sp -= 1; frame[sp] = 0;
    sp -= 1; frame[sp] = regs.ecx;
    sp -= 1; frame[sp] = regs.edx;
    sp -= 1; frame[sp] = regs.ebx;
    sp -= 1; frame[sp] = 0;
    sp -= 1; frame[sp] = regs.ebp;
    sp -= 1; frame[sp] = regs.esi;
    sp -= 1; frame[sp] = regs.edi;
    sp -= 1; frame[sp] = regs.ds;
    sp -= 1; frame[sp] = regs.es;

    let tramp = ffi::fork_task_trampoline as *const () as u32;
    sp -= 1; frame[sp] = tramp;
    sp -= 1; frame[sp] = 0;
    sp -= 1; frame[sp] = 0;
    sp -= 1; frame[sp] = 0;
    sp -= 1; frame[sp] = 0;

    child.esp = (stack_top - 20 * 4) as u32;

    // SAFETY: `child` is a live task; installing its sigreturn trampoline is part of fork.
    unsafe { task_setup_sigreturn(child as *mut TaskStruct) };

    // SAFETY: `child` is live and exclusively owned here.
    unsafe { task_list_add(child as *mut TaskStruct) };
    // SAFETY: `child` is live and `SCHEDULER_LOCK` is held.
    unsafe { mlfq::mlfq_enqueue_locked(child as *mut TaskStruct, child.priority) };

    // SAFETY: the lock was acquired above.
    unsafe { irq_spinlock_release(&raw mut SCHEDULER_LOCK) };

    child as *mut TaskStruct
}

/// # Safety
///
/// Must be called from the exiting task's own context with `SCHEDULER_LOCK` free.
#[no_mangle]
pub unsafe extern "C" fn sched_task_exit(exit_code: i32) {
    // SAFETY: `current_task` is a scheduler-owned global; the null case is checked just below.
    let t = unsafe { current_task };
    if t.is_null() {
        return;
    }
    // SAFETY: `t` is the live current task (non-null checked above).
    let p = unsafe { (*t).proc };

    // SAFETY: `SCHEDULER_LOCK` is the scheduler's global spinlock; this runs with it free.
    unsafe { irq_spinlock_acquire(&raw mut SCHEDULER_LOCK) };

    // SAFETY: `p` is the live current task's `ProcMeta`.
    unsafe { (*p).exit_code = exit_code };
    // SAFETY: `t` is the live current task.
    unsafe { (*t).state = TaskState::Zombie };

    // SAFETY: `t` is live.
    let my_pid = unsafe { (*t).pid };
    // SAFETY: `task_list_head` is a scheduler-owned global, read under `SCHEDULER_LOCK`.
    let mut child = unsafe { task_list_head };
    while !child.is_null() {
        // SAFETY: `child` is a live task on the list.
        let next_child = unsafe { (*child).next };
        // SAFETY: `child` is live, so its `proc` field is in bounds.
        let child_proc = unsafe { (*child).proc };
        // SAFETY: `child_proc` is that task's live `ProcMeta`.
        let child_parent = unsafe { (*child_proc).parent_pid };
        if child_parent == my_pid {
            // SAFETY: `child_proc` is live.
            unsafe { (*child_proc).parent_pid = 0 };
        }
        child = next_child;
    }

    // SAFETY: the lock was acquired above.
    unsafe { irq_spinlock_release(&raw mut SCHEDULER_LOCK) };

    // SAFETY: `schedule` is the scheduler's core switch routine; this task never runs again.
    unsafe { crate::mlfq::schedule() };
}

/// # Safety
///
/// `status` must be null or a writable `i32`; must be called from a task context.
#[no_mangle]
pub unsafe extern "C" fn sched_waitpid(target_pid: i32, status: *mut i32, options: i32) -> i32 {
    // WNOHANG: report instead of blocking while a matching child is still alive.
    const WNOHANG: i32 = 1;
    // WUNTRACED: report a child that a SIGSTOP has parked, not only deaths.
    const WUNTRACED: i32 = 2;
    // SIGSTOP's bit index, reported as the stopping signal (WSTOPSIG).
    const SIGSTOP_INDEX: i32 = 2;

    // SAFETY: `current_task` is a scheduler-owned global; the null case is checked below.
    let cur = unsafe { current_task };
    if cur.is_null() {
        return -1;
    }
    // SAFETY: `cur` is the live current task (non-null checked above).
    let cur_pid = unsafe { (*cur).pid };

    loop {
        // SAFETY: `SCHEDULER_LOCK` is the scheduler's global spinlock; it is released on every
        // exit path of this loop and re-acquired at the top.
        unsafe { irq_spinlock_acquire(&raw mut SCHEDULER_LOCK) };

        // SAFETY: `task_list_head` is a scheduler-owned global, read under `SCHEDULER_LOCK`.
        let mut t = unsafe { task_list_head };
        let mut found_child = false;

        while !t.is_null() {
            // SAFETY: `t` is a live task on the list.
            let t_proc = unsafe { (*t).proc };
            // SAFETY: `t_proc` is that task's live `ProcMeta`.
            let t_parent = unsafe { (*t_proc).parent_pid };
            // SAFETY: `t` is live.
            let t_pid = unsafe { (*t).pid };
            // SAFETY: `t` is live.
            let t_state = unsafe { (*t).state };
            if t_parent == cur_pid && (target_pid <= 0 || t_pid == target_pid as u32) {
                if matches!(t_state, TaskState::Zombie) {
                    let child_pid  = t_pid;
                    // SAFETY: `t_proc` is live.
                    let child_exit = unsafe { (*t_proc).exit_code };
                    // SAFETY: `t` is live and the lock is held.
                    unsafe { task_list_remove(t) };
                    let to_free = t;
                    // SAFETY: the lock was acquired above.
                    unsafe { irq_spinlock_release(&raw mut SCHEDULER_LOCK) };

                    reap_task_free(to_free);

                    if !status.is_null() {
                        // POSIX layout: exit code in bits 8-15, low byte clear.
                        // A raw code here would alias WIFSTOPPED for exit(127).
                        // SAFETY: `status` is non-null (checked here) and writable (caller
                        // contract).
                        unsafe { *status = (child_exit & 0xff) << 8 };
                    }
                    return child_pid as i32;
                }
                if matches!(t_state, TaskState::Stopped) && options & WUNTRACED != 0 {
                    let stopped_pid = t_pid;
                    // SAFETY: the lock was acquired above.
                    unsafe { irq_spinlock_release(&raw mut SCHEDULER_LOCK) };

                    if !status.is_null() {
                        // WIFSTOPPED: low byte 0x7f, stopping signal in bits 8-15.
                        // SAFETY: `status` is non-null (checked here) and writable (caller
                        // contract).
                        unsafe { *status = (SIGSTOP_INDEX << 8) | 0x7f };
                    }
                    return stopped_pid as i32;
                }
                found_child = true;
            }
            // SAFETY: `t` is live.
            t = unsafe { (*t).next };
        }

        if !found_child {
            // SAFETY: the lock was acquired above.
            unsafe { irq_spinlock_release(&raw mut SCHEDULER_LOCK) };
            return -1;
        }

        if options & WNOHANG != 0 {
            // SAFETY: the lock was acquired above.
            unsafe { irq_spinlock_release(&raw mut SCHEDULER_LOCK) };
            return 0;
        }

        // SAFETY: `cur` is the live current task.
        unsafe { (*cur).state = TaskState::Waiting };
        // SAFETY: the lock was acquired above.
        unsafe { irq_spinlock_release(&raw mut SCHEDULER_LOCK) };
        // SAFETY: `schedule` is the scheduler's core switch routine, called with the lock free.
        unsafe { crate::mlfq::schedule() };
    }
}

fn reap_task_free(t: *mut TaskStruct) {
    if t.is_null() {
        return;
    }
    // SAFETY: `t` is non-null and, per `task_reap`/`sched_waitpid`, a task already unlinked from
    // the scheduler list while holding `SCHEDULER_LOCK`, so it is no longer reachable and this
    // exclusive teardown cannot race another CPU.
    let proc_ptr = unsafe { (*t).proc };
    if proc_ptr.is_null() {
        return;
    }
    // SAFETY: `proc_ptr` is that task's live `ProcMeta`, exclusively owned by this teardown.
    let p = unsafe { &mut *proc_ptr };

    if !p.fds.is_null() {
        for j in 0..MAX_FD {
            // SAFETY: `p.fds` is the live fd table and `j < MAX_FD`.
            let ft = unsafe { (*p.fds).fd_table[j] };
            if !ft.is_null() {
                // SAFETY: `ft` is a live file object (a non-null slot of the table).
                unsafe { ffi::file_unref(ft as *mut c_void) };
            }
        }
        // SAFETY: `p.fds` is a live allocation owned by this dying task.
        unsafe { cact_mm::kfree((p.fds as *mut c_void) as *mut u8) };
        p.fds = ptr::null_mut();
    }

    if !p.mmap_table.is_null() {
        let mt = p.mmap_table;
        // SAFETY: `t` is live.
        let pd = unsafe { (*t).page_directory };
        if !pd.is_null() {
            // SAFETY: `mt` is the task's live mmap table and `pd` its live page directory.
            unsafe { cact_mm::mmap_table_free(mt, pd) };
        }
        // SAFETY: `mt` is a live allocation owned by this dying task.
        unsafe { cact_mm::kfree((mt as *mut c_void) as *mut u8) };
        p.mmap_table = ptr::null_mut();
    }

    // SAFETY: `t` is live.
    let pid = unsafe { (*t).pid };
    // SAFETY: `t` is live.
    let page_directory = unsafe { (*t).page_directory };
    cact_mm::shm_detach_all(pid, page_directory);
    // SAFETY: the tracker is part of this dying task's `ProcMeta`, exclusively owned here.
    unsafe { cact_mm::proc_free_pages(core::ptr::addr_of_mut!(p.mm)) };

    // SAFETY: `t` is live.
    unsafe { (*t).page_directory = ptr::null_mut() };
    p.ustack_phys    = ptr::null_mut();
    p.ustack_phys_extra = [ptr::null_mut(); 3];

    if !p.stack_base.is_null() {
        // SAFETY: `p.stack_base` is the task's live kernel stack, owned here.
        unsafe { kstack_free(p.stack_base) };
    }

    // SAFETY: `proc_ptr` is the dying task's live `ProcMeta`; `p` is no longer live here.
    unsafe { cact_mm::kfree((proc_ptr as *mut c_void) as *mut u8) };
    // SAFETY: `t` is the dying task's live `TaskStruct`, exclusively owned here.
    unsafe { cact_mm::kfree((t as *mut c_void) as *mut u8) };
}

/// # Safety
///
/// Must be called with `SCHEDULER_LOCK` free and the task list consistent.
#[no_mangle]
pub unsafe extern "C" fn task_reap() {
    let mut to_reap: [*mut TaskStruct; 64] = [ptr::null_mut(); 64];
    let mut count = 0usize;

    // SAFETY: `SCHEDULER_LOCK` is the scheduler's global spinlock; this runs with it free.
    unsafe { irq_spinlock_acquire(&raw mut SCHEDULER_LOCK) };
    // SAFETY: `task_list_head` is a scheduler-owned global, read under `SCHEDULER_LOCK`.
    let mut cur = unsafe { task_list_head };
    while !cur.is_null() && count < 64 {
        // SAFETY: `cur` is a live task on the list.
        let next = unsafe { (*cur).next };
        // SAFETY: `cur` is live.
        let cur_state = unsafe { (*cur).state };
        if matches!(cur_state, TaskState::Zombie) {
            // SAFETY: `cur` is live, so its `proc` field is in bounds.
            let cur_proc = unsafe { (*cur).proc };
            // SAFETY: `cur_proc` is that task's live `ProcMeta`.
            let parent_pid_val = unsafe { (*cur_proc).parent_pid };
            let reapable = parent_pid_val == 0
                || find_task_by_pid(parent_pid_val).is_null();
            if reapable {
                // SAFETY: `cur` is live and the lock is held.
                unsafe { task_list_remove(cur) };
                to_reap[count] = cur;
                count += 1;
            }
        }
        cur = next;
    }
    // SAFETY: the lock was acquired above.
    unsafe { irq_spinlock_release(&raw mut SCHEDULER_LOCK) };

    for t in &to_reap[..count] {
        reap_task_free(*t);
    }
}
