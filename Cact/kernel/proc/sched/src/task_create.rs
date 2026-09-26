//! Task creation: kernel tasks, plain user tasks, and ELF-backed processes
//! (static and dynamically linked).

use cact_mm::vmm_create_address_space;
use core::ffi::c_void;
use core::ptr;
use crate::ffi::{self, ProcPageTracker};
use crate::mlfq;
use crate::sync::{irq_spinlock_acquire, irq_spinlock_release};
use crate::task::{
    kstack_alloc, kstack_free, calc_highest_mapped_va, current_task, free_user_stack_pages, map_user_stack_in_pd,
    next_pid, push_empty_args, task_list_add, task_setup_sigreturn, task_zero_init,
    ustack_write_u32, ProcMeta, TaskStruct, SCHEDULER_LOCK, KERNEL_BASE, KERNEL_STACK_SIZE,
    USER_CODE_SEL, USER_DATA_SEL, USER_STACK_BYTES, USER_STACK_PAGES,
};

/// # Safety
///
/// `entry_point` must be a valid kernel entry trampoline for a new task; must be called with
/// interrupts disabled or the scheduler lock free.
#[no_mangle]
pub unsafe extern "C" fn create_task(entry_point: *const c_void) -> *mut TaskStruct {
    let t = cact_mm::kmalloc(core::mem::size_of::<TaskStruct>() as u32) as *mut TaskStruct;
    if t.is_null() {
        return ptr::null_mut();
    }

    let p = cact_mm::kmalloc(core::mem::size_of::<ProcMeta>() as u32) as *mut ProcMeta;
    if p.is_null() {
        // SAFETY: `t` is a live allocation owned here.
        unsafe { cact_mm::kfree((t as *mut c_void) as *mut u8) };
        return ptr::null_mut();
    }

    // SAFETY: `kstack_alloc` has no preconditions and returns a live stack or null.
    let stack = unsafe { kstack_alloc() };
    if stack.is_null() {
        // SAFETY: `p` is a live allocation owned here.
        unsafe { cact_mm::kfree((p as *mut c_void) as *mut u8) };
        // SAFETY: `t` is a live allocation owned here.
        unsafe { cact_mm::kfree((t as *mut c_void) as *mut u8) };
        return ptr::null_mut();
    }

    if !task_zero_init(t, p) {
        // SAFETY: `stack` is a live kernel stack owned here.
        unsafe { kstack_free(stack as *mut c_void) };
        // SAFETY: `p` is a live allocation owned here.
        unsafe { cact_mm::kfree((p as *mut c_void) as *mut u8) };
        // SAFETY: `t` is a live allocation owned here.
        unsafe { cact_mm::kfree((t as *mut c_void) as *mut u8) };
        return ptr::null_mut();
    }

    // SAFETY: `t`/`p` are fresh, exclusively owned structures initialised by `task_zero_init`.
    let t_ref = unsafe { &mut *t };
    // SAFETY: as above, for the `ProcMeta`.
    let p_ref = unsafe { &mut *p };

    let stack_top = stack as usize + KERNEL_STACK_SIZE;
    // SAFETY: `stack` is a live `KERNEL_STACK_SIZE`-byte kernel stack, so the top 6 words lie
    // inside it.
    let frame = unsafe { core::slice::from_raw_parts_mut((stack_top - 6 * 4) as *mut u32, 6) };
    let tramp = ffi::kernel_task_trampoline as *const () as u32;
    frame[5] = entry_point as u32;
    frame[4] = tramp;
    frame[3] = 0;
    frame[2] = 0;
    frame[1] = 0;
    frame[0] = 0;

    t_ref.esp = (stack_top - 6 * 4) as u32;
    // SAFETY: `next_pid` is a scheduler-owned global, read under `SCHEDULER_LOCK`.
    let pid = unsafe { next_pid };
    t_ref.pid = pid;
    // SAFETY: `next_pid` is a scheduler-owned global, incremented under `SCHEDULER_LOCK`.
    unsafe { next_pid = pid + 1 };
    t_ref.is_kernel      = 1;
    t_ref.page_directory = ptr::null_mut();
    p_ref.stack_base     = stack as *mut c_void;

    // SAFETY: `SCHEDULER_LOCK` is the scheduler's global spinlock.
    unsafe { irq_spinlock_acquire(&raw mut SCHEDULER_LOCK) };
    // SAFETY: `t_ref` is live and the lock is held.
    unsafe { task_list_add(t_ref as *mut TaskStruct) };
    // SAFETY: `t_ref` is live and the lock is held.
    unsafe { mlfq::mlfq_enqueue_locked(t_ref as *mut TaskStruct, t_ref.priority) };
    // SAFETY: the lock was acquired above.
    unsafe { irq_spinlock_release(&raw mut SCHEDULER_LOCK) };

    t_ref as *mut TaskStruct
}

fn create_user_task_internal(entry_point: *const c_void, add_to_list: bool) -> *mut TaskStruct {
    let t = cact_mm::kmalloc(core::mem::size_of::<TaskStruct>() as u32) as *mut TaskStruct;
    if t.is_null() {
        return ptr::null_mut();
    }

    let p = cact_mm::kmalloc(core::mem::size_of::<ProcMeta>() as u32) as *mut ProcMeta;
    if p.is_null() {
        // SAFETY: `t` is a live allocation owned here.
        unsafe { cact_mm::kfree((t as *mut c_void) as *mut u8) };
        return ptr::null_mut();
    }

    // SAFETY: `kstack_alloc` has no preconditions and returns a live stack or null.
    let kstack = unsafe { kstack_alloc() };
    if kstack.is_null() {
        // SAFETY: `p` and `t` are live allocations owned here.
        unsafe { cact_mm::kfree((p as *mut c_void) as *mut u8) };
        // SAFETY: `t` is a live allocation owned here.
        unsafe { cact_mm::kfree((t as *mut c_void) as *mut u8) };
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
            // SAFETY: `p` is a live allocation owned here.
            unsafe { cact_mm::kfree((p as *mut c_void) as *mut u8) };
            // SAFETY: `t` is a live allocation owned here.
            unsafe { cact_mm::kfree((t as *mut c_void) as *mut u8) };
            return ptr::null_mut();
        }
        ustack_pages[i] = page as *mut c_void;
    }

    if !task_zero_init(t, p) {
        for page in ustack_pages {
            // SAFETY: each entry is a page allocated by `kalloc` above.
            unsafe { cact_mm::free_page((page) as *mut u8) };
        }
        // SAFETY: `kstack` is a live kernel stack owned here.
        unsafe { kstack_free(kstack as *mut c_void) };
        // SAFETY: `p` is a live allocation owned here.
        unsafe { cact_mm::kfree((p as *mut c_void) as *mut u8) };
        // SAFETY: `t` is a live allocation owned here.
        unsafe { cact_mm::kfree((t as *mut c_void) as *mut u8) };
        return ptr::null_mut();
    }

    // SAFETY: `t`/`p` are fresh, exclusively owned structures initialised by `task_zero_init`.
    let t_ref = unsafe { &mut *t };
    // SAFETY: as above, for the `ProcMeta`.
    let p_ref = unsafe { &mut *p };

    let ustack_virt: u32 = KERNEL_BASE - USER_STACK_BYTES;

    let stack_top = kstack as usize + KERNEL_STACK_SIZE;
    // SAFETY: `kstack` is a live `KERNEL_STACK_SIZE`-byte kernel stack, so the top 10 words lie
    // inside it.
    let frame = unsafe { core::slice::from_raw_parts_mut((stack_top - 10 * 4) as *mut u32, 10) };
    let tramp = ffi::user_task_trampoline as *const () as u32;
    frame[9] = USER_DATA_SEL;
    frame[8] = ustack_virt + USER_STACK_BYTES - 4;
    frame[7] = 0x0000_0202;
    frame[6] = USER_CODE_SEL;
    frame[5] = entry_point as u32;
    frame[4] = tramp;
    frame[3] = 0;
    frame[2] = 0;
    frame[1] = 0;
    frame[0] = 0;

    t_ref.esp        = (stack_top - 10 * 4) as u32;
    p_ref.stack_base = kstack as *mut c_void;
    p_ref.ustack_phys = ustack_pages[0];
    p_ref.ustack_phys_extra = [ustack_pages[1], ustack_pages[2], ustack_pages[3]];
    p_ref.ustack_virt = ustack_virt;
    // SAFETY: `next_pid` is a scheduler-owned global, read under `SCHEDULER_LOCK`.
    let pid = unsafe { next_pid };
    t_ref.pid = pid;
    // SAFETY: `next_pid` is a scheduler-owned global, incremented under `SCHEDULER_LOCK`.
    unsafe { next_pid = pid + 1 };
    t_ref.is_kernel = 0;

    // SAFETY: `current_task` is a scheduler-owned global; the null case is handled below.
    let cur_raw = unsafe { current_task };
    if cur_raw.is_null() {
        p_ref.parent_pid = 0;
        p_ref.uid  = 0;
        p_ref.gid  = 0;
        p_ref.euid = 0;
        p_ref.egid = 0;
    } else {
        // SAFETY: `cur_raw` is the live current task (non-null checked here).
        let cur_proc = unsafe { (*cur_raw).proc };
        // SAFETY: `cur_proc` is that task's live `ProcMeta`.
        let cp = unsafe { &*cur_proc };
        // SAFETY: `cur_raw` is live.
        p_ref.parent_pid = unsafe { (*cur_raw).pid };
        p_ref.uid  = cp.uid;
        p_ref.gid  = cp.gid;
        p_ref.euid = cp.euid;
        p_ref.egid = cp.egid;
    }

    if add_to_list {
        // SAFETY: `SCHEDULER_LOCK` is the scheduler's global spinlock.
        unsafe { irq_spinlock_acquire(&raw mut SCHEDULER_LOCK) };
        // SAFETY: `t_ref` is live and the lock is held.
        unsafe { task_list_add(t_ref as *mut TaskStruct) };
        // SAFETY: `t_ref` is live and the lock is held.
        unsafe { mlfq::mlfq_enqueue_locked(t_ref as *mut TaskStruct, t_ref.priority) };
        // SAFETY: the lock was acquired above.
        unsafe { irq_spinlock_release(&raw mut SCHEDULER_LOCK) };
    }

    t_ref as *mut TaskStruct
}

/// # Safety
///
/// `entry_point` must point to a valid user entry stub; must be called from kernel context.
#[no_mangle]
pub unsafe extern "C" fn create_user_task(entry_point: *const c_void) -> *mut TaskStruct {
    create_user_task_internal(entry_point, true)
}

/// # Safety
///
/// `entry` must be a valid user entry point, `pd` a live page directory, and `tracker` a live
/// `ProcPageTracker`.
#[no_mangle]
pub unsafe extern "C" fn create_task_with_entry(
    entry:   *const c_void,
    pd:      *mut u32,
    tracker: *mut ProcPageTracker,
) -> *mut TaskStruct {
    let t = create_user_task_internal(entry, false);
    if t.is_null() {
        return ptr::null_mut();
    }
    // SAFETY: `t` is the freshly created user task (non-null checked above), exclusively owned
    // here.
    let t_ref = unsafe { &mut *t };
    // SAFETY: `t_ref.proc` was set by creation and points at a live `ProcMeta`.
    let p = unsafe { &mut *t_ref.proc };

    // SAFETY: `p` is the live `ProcMeta` and `tracker` the caller's live tracker (see # Safety).
    unsafe {
        ffi::memory_copy(
            core::ptr::addr_of_mut!(p.mm) as *mut c_void,
            tracker as *const c_void,
            core::mem::size_of::<ProcPageTracker>(),
        )
    };
    t_ref.page_directory = pd;

    // SAFETY: `t_ref.esp` points at the task's initial kernel-stack frame.
    let stk = t_ref.esp as *mut u32;
    // SAFETY: `stk` is that frame's base, so offset 5 (the saved EIP slot) is in bounds.
    let eip_slot = unsafe { stk.add(5) };
    // SAFETY: `eip_slot` is that in-bounds slot.
    unsafe { *eip_slot = entry as u32 };

    map_user_stack_in_pd(pd, p);

    let highest = calc_highest_mapped_va(pd);
    p.brk_start   = highest;
    p.brk_current = highest;

    let ustack_top = p.ustack_virt + USER_STACK_BYTES;
    let mut sp = ustack_top - 4;
    push_empty_args(p, &mut sp);
    // SAFETY: `stk` is the frame base, so offset 8 (the saved ESP slot) is in bounds.
    let esp_slot = unsafe { stk.add(8) };
    // SAFETY: `esp_slot` is that in-bounds slot.
    unsafe { *esp_slot = sp };

    // SAFETY: `t_ref` is live; installing its sigreturn trampoline is part of creation.
    unsafe { task_setup_sigreturn(t_ref as *mut TaskStruct) };

    // SAFETY: `SCHEDULER_LOCK` is the scheduler's global spinlock.
    unsafe { irq_spinlock_acquire(&raw mut SCHEDULER_LOCK) };
    // SAFETY: `t_ref` is live and the lock is held.
    unsafe { task_list_add(t_ref as *mut TaskStruct) };
    // SAFETY: `t_ref` is live and the lock is held.
    unsafe { mlfq::mlfq_enqueue_locked(t_ref as *mut TaskStruct, t_ref.priority) };
    // SAFETY: the lock was acquired above.
    unsafe { irq_spinlock_release(&raw mut SCHEDULER_LOCK) };

    t_ref as *mut TaskStruct
}

/// # Safety
///
/// `path` must be a valid NUL-terminated path string in kernel memory.
#[no_mangle]
pub unsafe extern "C" fn create_elf_task(path: *const u8) -> *mut TaskStruct {
    let pd = vmm_create_address_space();
    if pd.is_null() {
        return ptr::null_mut();
    }

    let t = create_user_task_internal(ptr::null(), false);
    if t.is_null() {
        // SAFETY: `pd` is a live address space owned here.
        unsafe { cact_mm::vmm_free_address_space(pd) };
        return ptr::null_mut();
    }

    // SAFETY: `t` is the freshly created user task (non-null checked above), exclusively owned
    // here.
    let t_ref = unsafe { &mut *t };
    // SAFETY: `t_ref.proc` was set by creation and points at a live `ProcMeta`.
    let p = unsafe { &mut *t_ref.proc };
    // SAFETY: the pointer is derived from `p`, so the tracker reset is in bounds.
    unsafe { ffi::proc_tracker_init(core::ptr::addr_of_mut!(p.mm)) };

    // PT_INTERP handoff (userspace ld.so) — same protocol as task_exec.
    // Binaries without PT_INTERP are mapped by the plain loader (static ELF).
    let mut interp_path = [0u8; 256];
    // SAFETY: `path` is a NUL-terminated kernel string and `interp_path` a live 256-byte buffer.
    let has_interp =
        unsafe { ffi::elf_get_interp_path(path, interp_path.as_mut_ptr(), 256) } > 0;
    let mut interp_info = ffi::InterpInfo {
        main_entry:  0,
        main_base:   0,
        main_phdr:   0,
        main_phnum:  0,
        interp_base: 0,
    };

    let entry = if has_interp {
        // SAFETY: all pointers are live: `path`/`interp_path` are strings, `pd` the address
        // space just created, `p.mm` the task's tracker and `interp_info` a live local.
        unsafe {
            ffi::load_elf_interp(
                path,
                interp_path.as_ptr(),
                pd,
                core::ptr::addr_of_mut!(p.mm),
                core::ptr::addr_of_mut!(interp_info),
            )
        }
    } else {
        // SAFETY: as above, without the interpreter.
        unsafe { ffi::load_elf(path, pd, core::ptr::addr_of_mut!(p.mm)) }
    };
    if entry.is_null() {
        // SAFETY: `p.stack_base` is this task's live kernel stack.
        unsafe { kstack_free(p.stack_base) };
        free_user_stack_pages(p);
        // SAFETY: `p` is a live allocation owned here.
        unsafe { cact_mm::kfree((p as *mut ProcMeta as *mut c_void) as *mut u8) };
        // SAFETY: `t` is a live allocation owned here.
        unsafe { cact_mm::kfree((t as *mut c_void) as *mut u8) };
        // SAFETY: `pd` is a live address space owned here.
        unsafe { cact_mm::vmm_free_address_space(pd) };
        return ptr::null_mut();
    }

    t_ref.page_directory = pd;

    // Load symbol table for crash traces
    // SAFETY: `path` is a live string and `p` this task's live `ProcMeta`.
    unsafe { ffi::elf_load_exec_symtab(path, p as *mut ProcMeta as *mut c_void) };

    // SAFETY: `t_ref.esp` points at the task's initial kernel-stack frame.
    let stk = t_ref.esp as *mut u32;
    // SAFETY: `stk` is the frame base, so offset 5 (the saved EIP slot) is in bounds.
    let eip_slot = unsafe { stk.add(5) };
    // SAFETY: `eip_slot` is that in-bounds slot.
    unsafe { *eip_slot = entry as u32 };

    map_user_stack_in_pd(pd, p);

    let highest = calc_highest_mapped_va(pd);
    if has_interp {
        // With the interpreter mapped too, calc_highest lands above ld.so;
        // the brk must start at the end of the *main* image instead.
        // SAFETY: `vfs_root` is the kernel's VFS root global.
        let root = unsafe { ffi::vfs_root.get() };
        // SAFETY: `root` points at the kernel's VFS root node.
        let root_node = unsafe { *root };
        // SAFETY: `root_node` is a live VFS node and `path` a live string.
        let main_node = unsafe { ffi::vfs_walk_path(root_node, path) };
        let brk = if !main_node.is_null() {
            // SAFETY: `main_node` is a live VFS node (non-null checked here).
            unsafe { ffi::elf_get_brk_start(main_node) }
        } else {
            highest
        };
        p.brk_start   = brk;
        p.brk_current = brk;
    } else {
        p.brk_start   = highest;
        p.brk_current = highest;
    }

    let ustack_top = p.ustack_virt + USER_STACK_BYTES;
    let mut sp = ustack_top - 4;

    // auxv goes between envp's NULL and the top of the stack (init has no
    // argv/envp strings, so it sits directly above the empty envp array).
    if has_interp {
        const AT_PHDR: u32 = 3;
        const AT_PHENT: u32 = 4;
        const AT_PHNUM: u32 = 5;
        const AT_PAGESZ: u32 = 6;
        const AT_BASE: u32 = 7;
        const AT_ENTRY: u32 = 9;
        let auxv: [(u32, u32); 6] = [
            (AT_PHDR, interp_info.main_phdr),
            (AT_PHENT, 32),
            (AT_PHNUM, interp_info.main_phnum),
            (AT_PAGESZ, 4096),
            (AT_BASE, interp_info.interp_base),
            (AT_ENTRY, interp_info.main_entry),
        ];
        sp -= 4; ustack_write_u32(p, sp, 0); // auxv terminator (val)
        sp -= 4; ustack_write_u32(p, sp, 0); // auxv terminator (tag)
        for i in (0..6).rev() {
            sp -= 4; ustack_write_u32(p, sp, auxv[i].1);
            sp -= 4; ustack_write_u32(p, sp, auxv[i].0);
        }
    }

    push_empty_args(p, &mut sp);
    // SAFETY: `stk` is the frame base, so offset 8 (the saved ESP slot) is in bounds.
    let esp_slot = unsafe { stk.add(8) };
    // SAFETY: `esp_slot` is that in-bounds slot.
    unsafe { *esp_slot = sp };

    // SAFETY: `t_ref` is live; installing its sigreturn trampoline is part of creation.
    unsafe { task_setup_sigreturn(t_ref as *mut TaskStruct) };

    // SAFETY: `SCHEDULER_LOCK` is the scheduler's global spinlock.
    unsafe { irq_spinlock_acquire(&raw mut SCHEDULER_LOCK) };
    // SAFETY: `t_ref` is live and the lock is held.
    unsafe { task_list_add(t_ref as *mut TaskStruct) };
    // SAFETY: `t_ref` is live and the lock is held.
    unsafe { mlfq::mlfq_enqueue_locked(t_ref as *mut TaskStruct, t_ref.priority) };
    // SAFETY: the lock was acquired above.
    unsafe { irq_spinlock_release(&raw mut SCHEDULER_LOCK) };

    t_ref as *mut TaskStruct
}
