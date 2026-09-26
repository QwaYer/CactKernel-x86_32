//! `execve`-style task replacement: resolve + load a new ELF image into a
//! fresh address space, copy argv/envp into the new user stack, and iretd.

use cact_mm::vmm_create_address_space;
use core::ffi::c_void;
use core::ptr;
use crate::ffi::{self, ContextFrame, PAGE_SIZE};
use crate::sync::{irq_spinlock_acquire, irq_spinlock_release};
use crate::task::{
    map_sigreturn_trampoline_on_pd, map_user_stack_in_pd, ustack_kernel_byte_mut,
    ustack_write_u32, ProcMeta, EXEC_MAX_ARGS, EXEC_MAX_ENVS,
    EXEC_MAX_STRLEN, KERNEL_STACK_SIZE, MAX_FD, NSIG, SCHEDULER_LOCK, SIG_DFL,
    TASK_SHM_MAX, USER_STACK_BYTES, USER_STACK_PAGES, current_task,
};

/// C `file_t` layout: { node*, offset, flags, cloexec, refcount }.
#[inline]
fn exec_cloexec(ft: *mut c_void) -> bool {
    if ft.is_null() {
        return false;
    }
    // SAFETY: `ft` points to a live `file_t`, so the 4th `u32` word is in bounds.
    let cloexec_ptr = unsafe { ft.cast::<u32>().add(3) };
    // SAFETY: `cloexec_ptr` is that in-bounds word.
    let cloexec = unsafe { *cloexec_ptr };
    cloexec & 1 != 0
}

/// # Safety
///
/// `path` must be a valid NUL-terminated path; `argv`/`envp` may be null or NUL-terminated
/// pointer arrays; `regs` may be null.
#[no_mangle]
pub unsafe extern "C" fn task_exec(
    path: *const u8,
    argv: *mut *mut u8,
    envp: *mut *mut u8,
    regs: *mut ContextFrame,
) -> i32 {
    let _ = regs;

    if path.is_null() {
        return -1;
    }

    // SAFETY: `current_task` is a scheduler-owned global; the null case is checked below.
    let t = unsafe { current_task };
    if t.is_null() {
        return -1;
    }
    // SAFETY: `t` is the live current task (non-null checked above).
    let t_is_kernel = unsafe { (*t).is_kernel };
    if t_is_kernel != 0 {
        return -1;
    }
    // SAFETY: `t` is live, so its `proc` field is in bounds.
    let p = unsafe { (*t).proc };

    // SAFETY: `p` is the live current task's `ProcMeta`.
    let old_ustack_phys = unsafe { (*p).ustack_phys };
    // SAFETY: `p` is live, so the saved-page array is in bounds.
    let old_ustack_extra = unsafe { (*p).ustack_phys_extra };
    let old_ustack_pages = [
        old_ustack_phys,
        old_ustack_extra[0],
        old_ustack_extra[1],
        old_ustack_extra[2],
    ];

    let mut new_ustack_pages: [*mut c_void; USER_STACK_PAGES as usize] =
        [ptr::null_mut(); USER_STACK_PAGES as usize];
    for i in 0..USER_STACK_PAGES as usize {
        let page = cact_mm::kalloc();
        if page.is_null() {
            for page in &new_ustack_pages[..i] {
                // SAFETY: each entry is a page allocated by `kalloc` above.
                unsafe { cact_mm::free_page((*page) as *mut u8) };
            }
            return -1;
        }
        new_ustack_pages[i] = page as *mut c_void;
    }

    // SAFETY: `SCHEDULER_LOCK` is the scheduler's global spinlock; the caller must not hold it.
    unsafe { irq_spinlock_acquire(&raw mut SCHEDULER_LOCK) };

    // dynlink is gone; the old address space's frames are reclaimed when
    // old_pd is released below.  Free the stale tracker array here so execs
    // do not leak it.
    // SAFETY: `p` is the live current task's `ProcMeta`.
    let old_tracker_pages = unsafe { (*p).mm.pages };
    if !old_tracker_pages.is_null() {
        // SAFETY: `old_tracker_pages` is the tracker array owned by this task.
        unsafe { cact_mm::kfree((old_tracker_pages as *mut c_void) as *mut u8) };
    }

    let new_pd = vmm_create_address_space();
    if new_pd.is_null() {
        for page in new_ustack_pages {
            // SAFETY: each entry is a page allocated by `kalloc` above.
            unsafe { cact_mm::free_page((page) as *mut u8) };
        }
        // SAFETY: the lock was acquired above.
        unsafe { irq_spinlock_release(&raw mut SCHEDULER_LOCK) };
        return -1;
    }

    // SAFETY: the pointer is derived from `p`, so the tracker reset is in bounds.
    let mm_ptr = unsafe { core::ptr::addr_of_mut!((*p).mm) };
    // SAFETY: `mm_ptr` is the live current task's page tracker.
    unsafe { ffi::proc_tracker_init(mm_ptr) };
    // SAFETY: the lock was acquired above.
    unsafe { irq_spinlock_release(&raw mut SCHEDULER_LOCK) };

    // PT_INTERP handoff (userspace ld.so): the kernel maps the main image and
    // the interpreter without any relocation pass; ld.so does the rest.
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

    // Check execute permission before loading
    {
        // SAFETY: `vfs_root` is the kernel's VFS root global.
        let root = unsafe { ffi::vfs_root.get() };
        // SAFETY: `root` points at the kernel's VFS root node.
        let root_node = unsafe { *root };
        // SAFETY: `root_node` is a live VFS node and `path` a live string.
        let exec_node = unsafe { ffi::vfs_walk_path(root_node, path) };
        // SAFETY: `exec_node` is null or a live VFS node.
        let perm_ok = !exec_node.is_null()
            && unsafe { ffi::vfs_check_perm(exec_node, 0x01) } >= 0;
        if !perm_ok {
            // SAFETY: `new_pd` is a live address space owned here.
            unsafe { cact_mm::vmm_free_address_space(new_pd) };
            for page in new_ustack_pages {
                // SAFETY: each entry is a page allocated by `kalloc` above.
                unsafe { cact_mm::free_page((page) as *mut u8) };
            }
            return -1;
        }
    }

    let entry = if has_interp {
        // SAFETY: `p` is the live current task's `ProcMeta`, so this tracker address is in
        // bounds.
        let mm_ptr = unsafe { core::ptr::addr_of_mut!((*p).mm) };
        // SAFETY: `path`/`interp_path` are strings, `new_pd` the address space just created,
        // `mm_ptr` this task's tracker and `interp_info` a live local.
        unsafe {
            ffi::load_elf_interp(
                path,
                interp_path.as_ptr(),
                new_pd,
                mm_ptr,
                core::ptr::addr_of_mut!(interp_info),
            )
        }
    } else {
        // SAFETY: `p` is the live current task's `ProcMeta`, so this tracker address is in
        // bounds.
        let mm_ptr = unsafe { core::ptr::addr_of_mut!((*p).mm) };
        // SAFETY: `path` is a live string, `new_pd` the fresh address space and `mm_ptr` this
        // task's tracker.
        unsafe { ffi::load_elf(path, new_pd, mm_ptr) }
    };
    if entry.is_null() {
        // SAFETY: `new_pd` is a live address space owned here.
        unsafe { cact_mm::vmm_free_address_space(new_pd) };
        for page in new_ustack_pages {
            // SAFETY: each entry is a page allocated by `kalloc` above.
            unsafe { cact_mm::free_page((page) as *mut u8) };
        }
        return -1;
    }

    // Load symbol table for crash traces
    // SAFETY: `path` is a live string and `p` the live task's `ProcMeta`.
    unsafe { ffi::elf_load_exec_symtab(path, p as *mut c_void) };

    // SAFETY: `SCHEDULER_LOCK` is the scheduler's global spinlock; it was released above.
    unsafe { irq_spinlock_acquire(&raw mut SCHEDULER_LOCK) };

    // SAFETY: `p` is the live current task's `ProcMeta`.
    unsafe { (*p).ustack_phys = new_ustack_pages[0] };
    // SAFETY: `p` is live.
    unsafe {
        (*p).ustack_phys_extra = [new_ustack_pages[1], new_ustack_pages[2], new_ustack_pages[3]]
    };
    // SAFETY: `p` is the live current task's `ProcMeta`.
    map_user_stack_in_pd(new_pd, unsafe { &*p });

    for page in &new_ustack_pages {
        let us = *page as *mut u8;
        // SAFETY: `us` is a page allocated by `kalloc` above.
        unsafe { ffi::memory_set(us as *mut c_void, 0, PAGE_SIZE as usize) };
    }

    {
        // SAFETY: `vfs_root` is the kernel's VFS root global.
        let root = unsafe { ffi::vfs_root.get() };
        // SAFETY: `root` points at the kernel's VFS root node.
        let root_node = unsafe { *root };
        // SAFETY: `root_node` is a live VFS node and `path` a live string.
        let file = unsafe { ffi::vfs_walk_path(root_node, path) };
        if !file.is_null() {
            // SAFETY: `file` is a live VFS node (non-null checked here).
            let brk = unsafe { ffi::elf_get_brk_start(file) };
            // SAFETY: `p` is the live current task's `ProcMeta`.
            unsafe { (*p).brk_start = brk };
            // SAFETY: `p` is live.
            unsafe { (*p).brk_current = brk };
        }
    }

    map_sigreturn_trampoline_on_pd(t, new_pd);
    // SAFETY: `new_pd` is a live address space.
    unsafe { cact_mm::vmm_sync_kernel_mmio_mappings(new_pd) };

    // SAFETY: `p` is the live current task's `ProcMeta`.
    let stack_base = unsafe { (*p).stack_base };
    let esp0 = stack_base as u32 + KERNEL_STACK_SIZE as u32;
    // SAFETY: `tss_entry` is the kernel TSS global.
    let tss_ptr = unsafe { ffi::tss_entry.get() };
    // SAFETY: `tss_ptr` points at the kernel TSS.
    unsafe { (*tss_ptr).esp0 = esp0 };
    // SAFETY: sets the ring-0 stack for the upcoming user entry.
    unsafe { ffi::syscall_set_esp0(esp0) };

    // SAFETY: `p` is the live current task's `ProcMeta`.
    let ustack_virt = unsafe { (*p).ustack_virt };
    let ustack_top = ustack_virt + USER_STACK_BYTES;
    let mut sp     = ustack_top - 4;

    let mut argv_vaddrs: [u32; 256] = [0; 256];
    let mut envp_vaddrs: [u32; 256] = [0; 256];

    // SAFETY: `p` is the live current task's `ProcMeta`.
    let argc = match copy_strings_to_ustack(argv, EXEC_MAX_ARGS, unsafe { &*p }, &mut sp, &mut argv_vaddrs) {
        Some(n) => n,
        None => {
            // SAFETY: `printk` takes a static NUL-terminated byte string.
            unsafe { ffi::printk(c"[EXEC] abort: argv copy / stack overflow\n".as_ptr().cast()) };
            // SAFETY: `p` is the live current task's `ProcMeta`.
            unsafe { (*p).ustack_phys = old_ustack_pages[0] };
            // SAFETY: `p` is live.
            unsafe {
                (*p).ustack_phys_extra =
                    [old_ustack_pages[1], old_ustack_pages[2], old_ustack_pages[3]]
            };
            // SAFETY: `new_pd` is a live address space owned here.
            unsafe { cact_mm::vmm_free_address_space(new_pd) };
            // SAFETY: the lock was acquired above.
            unsafe { irq_spinlock_release(&raw mut SCHEDULER_LOCK) };
            return -1;
        }
    };
    // SAFETY: `p` is the live current task's `ProcMeta`.
    let envc = match copy_strings_to_ustack(envp, EXEC_MAX_ENVS, unsafe { &*p }, &mut sp, &mut envp_vaddrs) {
        Some(n) => n,
        None => {
            // SAFETY: `printk` takes a static NUL-terminated byte string.
            unsafe { ffi::printk(c"[EXEC] abort: envp copy / stack overflow\n".as_ptr().cast()) };
            // SAFETY: `p` is the live current task's `ProcMeta`.
            unsafe { (*p).ustack_phys = old_ustack_pages[0] };
            // SAFETY: `p` is live.
            unsafe {
                (*p).ustack_phys_extra =
                    [old_ustack_pages[1], old_ustack_pages[2], old_ustack_pages[3]]
            };
            // SAFETY: `new_pd` is a live address space owned here.
            unsafe { cact_mm::vmm_free_address_space(new_pd) };
            // SAFETY: the lock was acquired above.
            unsafe { irq_spinlock_release(&raw mut SCHEDULER_LOCK) };
            return -1;
        }
    };

    // auxv pairs (Linux i386 stack layout): written just above envp's NULL,
    // below the argv/envp string area. Only the PT_INTERP path uses them;
    // kernel-dynlink binaries keep the plain stack.
    const AT_PHDR: u32 = 3;
    const AT_PHENT: u32 = 4;
    const AT_PHNUM: u32 = 5;
    const AT_PAGESZ: u32 = 6;
    const AT_BASE: u32 = 7;
    const AT_ENTRY: u32 = 9;
    let mut auxv: [(u32, u32); 8] = [(0, 0); 8];
    let mut auxc = 0usize;
    if has_interp {
        auxv[auxc] = (AT_PHDR, interp_info.main_phdr); auxc += 1;
        auxv[auxc] = (AT_PHENT, 32);                  auxc += 1; // sizeof(Elf32_Phdr)
        auxv[auxc] = (AT_PHNUM, interp_info.main_phnum); auxc += 1;
        auxv[auxc] = (AT_PAGESZ, 4096);               auxc += 1;
        auxv[auxc] = (AT_BASE, interp_info.interp_base); auxc += 1;
        auxv[auxc] = (AT_ENTRY, interp_info.main_entry); auxc += 1;
    }
    if auxc > 0 {
        sp -= 4;
        // SAFETY: `p` is the live current task's `ProcMeta`.
        ustack_write_u32(unsafe { &*p }, sp, 0); // auxv terminator (val)
        sp -= 4;
        // SAFETY: `p` is the live current task's `ProcMeta`.
        ustack_write_u32(unsafe { &*p }, sp, 0); // auxv terminator (tag)
        for i in (0..auxc).rev() {
            sp -= 4;
            // SAFETY: `p` is the live current task's `ProcMeta`.
            ustack_write_u32(unsafe { &*p }, sp, auxv[i].1);
            sp -= 4;
            // SAFETY: `p` is the live current task's `ProcMeta`.
            ustack_write_u32(unsafe { &*p }, sp, auxv[i].0);
        }
    }

    // The pointer-layout block below stays identical for both exec paths.
    let ptr_overhead = (argc as u32 + envc as u32 + 5) * 4;
    if sp < ustack_virt + ptr_overhead {
        // SAFETY: `printk` takes a static NUL-terminated byte string.
        unsafe { ffi::printk(c"[EXEC] abort: stack layout preflight failed\n".as_ptr().cast()) };
        // SAFETY: `p` is the live current task's `ProcMeta`.
        unsafe { (*p).ustack_phys = old_ustack_pages[0] };
        // SAFETY: `p` is live.
        unsafe {
            (*p).ustack_phys_extra = [old_ustack_pages[1], old_ustack_pages[2], old_ustack_pages[3]]
        };
        // SAFETY: `new_pd` is a live address space owned here.
        unsafe { cact_mm::vmm_free_address_space(new_pd) };
        // SAFETY: the lock was acquired above.
        unsafe { irq_spinlock_release(&raw mut SCHEDULER_LOCK) };
        return -1;
    }

    sp -= 4;
    // SAFETY: `p` is the live current task's `ProcMeta`.
    ustack_write_u32(unsafe { &*p }, sp, 0);
    for i in (0..envc).rev() {
        sp -= 4;
        // SAFETY: `p` is the live current task's `ProcMeta`.
        ustack_write_u32(unsafe { &*p }, sp, envp_vaddrs[i]);
    }
    let envp_arr = sp;

    sp -= 4;
    // SAFETY: `p` is the live current task's `ProcMeta`.
    ustack_write_u32(unsafe { &*p }, sp, 0);
    for i in (0..argc).rev() {
        sp -= 4;
        // SAFETY: `p` is the live current task's `ProcMeta`.
        ustack_write_u32(unsafe { &*p }, sp, argv_vaddrs[i]);
    }
    let argv_arr = sp;

    sp -= 4;
    // SAFETY: `p` is the live current task's `ProcMeta`.
    ustack_write_u32(unsafe { &*p }, sp, envp_arr);
    sp -= 4;
    // SAFETY: `p` is the live current task's `ProcMeta`.
    ustack_write_u32(unsafe { &*p }, sp, argv_arr);
    sp -= 4;
    // SAFETY: `p` is the live current task's `ProcMeta`.
    ustack_write_u32(unsafe { &*p }, sp, argc as u32);

    // SAFETY: `p` is the live current task's `ProcMeta`.
    unsafe { (*p).pending_signals = 0 };
    for i in 0..NSIG {
        // SAFETY: `p` is the live current task's `ProcMeta` and `i < NSIG`.
        unsafe { (*p).signal_handlers[i] = SIG_DFL };
    }

    // SAFETY: `t` is live.
    let t_pid = unsafe { (*t).pid };
    cact_mm::shm_detach_all(t_pid, new_pd);
    for i in 0..TASK_SHM_MAX {
        // SAFETY: `p` is the live current task's `ProcMeta` and `i < TASK_SHM_MAX`.
        unsafe {
            (*p).shm_attachments[i].shm_id    = 0;
        }
        // SAFETY: `p` is live.
        unsafe {
            (*p).shm_attachments[i].shm_vaddr = 0;
        }
    }

    // SAFETY: `p` is the live current task's `ProcMeta`.
    let mmap_table = unsafe { (*p).mmap_table };
    // SAFETY: `mmap_table` is this task's live mmap table.
    unsafe { cact_mm::mmap_table_init(mmap_table) };

    for i in 3..MAX_FD {
        // SAFETY: `p` is the live current task's `ProcMeta`.
        let fds = unsafe { (*p).fds };
        // SAFETY: `fds` is this task's live fd table and `i < MAX_FD`.
        let ft = unsafe { (*fds).fd_table[i] };
        if !ft.is_null() && exec_cloexec(ft as *mut c_void) {
            // SAFETY: `ft` is a live file object.
            unsafe { ffi::file_unref(ft as *mut c_void) };
            // SAFETY: `p` is live.
            let fds_mut = unsafe { (*p).fds };
            // SAFETY: `fds_mut` is this task's live fd table.
            unsafe { (*(fds_mut)).fd_table[i] = ptr::null_mut() };
        }
    }

    // SAFETY: `p` is the live current task's `ProcMeta`.
    map_user_stack_in_pd(new_pd, unsafe { &*p });

    // SAFETY: `t` is live.
    let old_pd = unsafe { (*t).page_directory };
    // SAFETY: `t` is live.
    unsafe { (*t).page_directory = new_pd };
    // SAFETY: switches CR3 to the freshly loaded address space.
    unsafe { ffi::switch_paging(new_pd) };
    if !old_pd.is_null() {
        // SAFETY: `old_pd` is this task's previous, now-unreferenced address space.
        unsafe { cact_mm::vmm_free_address_space(old_pd) };
    }
    // SAFETY: `new_pd` is a live address space.
    unsafe { cact_mm::vmm_sync_kernel_mmio_mappings(new_pd) };

    // SAFETY: `invlpg` on the freshly mapped `entry` page is a TLB maintenance instruction
    // issued while the scheduler lock is held; it reads no memory and only invalidates one
    // TLB entry.
    unsafe {
        core::arch::asm!(
            "invlpg [{}]",
            in(reg) entry as u32,
            options(nostack),
        );
    }

    // SAFETY: the lock was acquired above.
    unsafe { irq_spinlock_release(&raw mut SCHEDULER_LOCK) };

    // SAFETY: `terminal_fg_pid` is a kernel global for the console's foreground pid.
    let fg_pid_ptr = unsafe { ffi::terminal_fg_pid.get() };
    // SAFETY: `t` is the live current task.
    let fg_pid = unsafe { (*t).pid };
    // SAFETY: `fg_pid_ptr` points at the console's foreground-pid global.
    unsafe { *fg_pid_ptr = fg_pid };

    let pd_val = new_pd as u32;
    let entry_u = entry as u32;
    let sp_u = sp;

    // SAFETY: the final exec commit: `new_pd` is the just-loaded address space, `sp_u` a
    // stack pointer inside its mapped user stack and `entry_u` a present user-mapped entry
    // point; `cr3`/segment reload and `iretd` leave kernel mode permanently (`noreturn`).
    unsafe {
        core::arch::asm!(
            "mov eax, {pd:e}",
            "mov cr3, eax",
            "xor ebx, ebx",
            "mov eax, 0x23",
            "mov ds, ax",
            "mov es, ax",
            "mov fs, ax",
            "mov gs, ax",
            "push 0x23",
            "push ecx",
            "push 0x202",
            "push 0x1B",
            "push edx",
            "iretd",
            pd = in(reg) pd_val,
            in("ecx") sp_u,
            in("edx") entry_u,
            options(noreturn),
        );
    }
}

fn copy_strings_to_ustack(
    src:    *mut *mut u8,
    max:    usize,
    p:      &ProcMeta,
    sp:     &mut u32,
    vaddrs: &mut [u32; 256],
) -> Option<usize> {
    if src.is_null() {
        return Some(0);
    }
    // SAFETY: `src` is a caller-supplied NUL-terminated pointer array; each element is
    // null-checked, every string length is bounded by `EXEC_MAX_STRLEN`, and each byte is written
    // through `ustack_kernel_byte_mut`, which bounds its offset against `USER_STACK_BYTES`.
    let mut count = 0usize;
    for (i, vaddr) in vaddrs.iter_mut().enumerate().take(max) {
        // SAFETY: `src` is a caller-supplied NUL-terminated pointer array and `i < max`.
        let s_holder = unsafe { src.add(i) };
        // SAFETY: `s_holder` is an in-bounds element of that array.
        let s = unsafe { *s_holder };
        if s.is_null() {
            break;
        }

        let mut len = 0usize;
        loop {
            if len >= EXEC_MAX_STRLEN {
                break;
            }
            // SAFETY: the string is bounded by `EXEC_MAX_STRLEN` bytes (caller contract), so
            // `len` stays in bounds while it is below that limit.
            let holder = unsafe { s.add(len) };
            // SAFETY: `holder` is that in-bounds byte.
            if unsafe { *holder } == 0 {
                break;
            }
            len += 1;
        }
        if len >= EXEC_MAX_STRLEN {
            return None;
        }

        if *sp < p.ustack_virt + (len + 1 + count * 4) as u32 {
            return None;
        }

        *sp = (*sp).wrapping_sub(len as u32 + 1);
        for j in 0..=len {
            // SAFETY: the string is bounded by `len`, so `j <= len` is in bounds.
            let byte_ptr = unsafe { s.add(j) };
            // SAFETY: `byte_ptr` is that in-bounds byte.
            let byte = unsafe { *byte_ptr };
            // SAFETY: `ustack_kernel_byte_mut` bounds its offset against `USER_STACK_BYTES`.
            unsafe { *(ustack_kernel_byte_mut(p, *sp + j as u32)) = byte };
        }
        *vaddr = *sp;
        count += 1;
    }
    Some(count)
}
