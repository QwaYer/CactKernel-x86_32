//! `execve`-style task replacement: resolve + load a new ELF image into a
//! fresh address space, copy argv/envp into the new user stack, and iretd.

use core::ffi::c_void;
use core::ptr;
use crate::ffi::{self, ContextFrame, PAGE_PRESENT, PAGE_SIZE, PAGE_USER};
use crate::sync::{irq_spinlock_acquire, irq_spinlock_release};
use crate::task::{
    map_sigreturn_trampoline_on_pd, map_user_stack_in_pd, ustack_kernel_byte_mut,
    ustack_write_u32, ProcMeta, TRACE_PROC_LOGS, EXEC_MAX_ARGS, EXEC_MAX_ENVS,
    EXEC_MAX_STRLEN, KERNEL_STACK_SIZE, MAX_FD, NSIG, SCHEDULER_LOCK, SIG_DFL,
    TASK_SHM_MAX, USER_STACK_BYTES, USER_STACK_PAGES, current_task,
};

#[no_mangle]
pub unsafe extern "C" fn task_exec(
    path: *const u8,
    argv: *mut *mut u8,
    envp: *mut *mut u8,
    regs: *mut ContextFrame,
) -> i32 {
    let _ = regs;

    if path.is_null() { return -1; }

    let t = current_task;
    if t.is_null() || (*t).is_kernel != 0 { return -1; }

    let p = (*t).proc;

    let old_ustack_pages = [
        (*p).ustack_phys,
        (*p).ustack_phys_extra[0],
        (*p).ustack_phys_extra[1],
        (*p).ustack_phys_extra[2],
    ];
    let mut new_ustack_pages: [*mut c_void; USER_STACK_PAGES as usize] =
        [ptr::null_mut(); USER_STACK_PAGES as usize];
    for i in 0..USER_STACK_PAGES as usize {
        let page = ffi::kalloc() as *mut c_void;
        if page.is_null() {
            for j in 0..i {
                ffi::free_page(new_ustack_pages[j]);
            }
            return -1;
        }
        new_ustack_pages[i] = page;
    }

    irq_spinlock_acquire(&raw mut SCHEDULER_LOCK);

    // dynlink is gone; the old address space's frames are reclaimed when
    // old_pd is released below.  Free the stale tracker array here so execs
    // do not leak it.
    if !(*p).mm.pages.is_null() {
        ffi::kfree((*p).mm.pages as *mut c_void);
    }

    let new_pd = ffi::vmm_create_address_space();
    if new_pd.is_null() {
        for page in new_ustack_pages {
            ffi::free_page(page);
        }
        irq_spinlock_release(&raw mut SCHEDULER_LOCK);
        return -1;
    }

    ffi::proc_tracker_init(&raw mut (*p).mm);
    irq_spinlock_release(&raw mut SCHEDULER_LOCK);

    // PT_INTERP handoff (userspace ld.so): the kernel maps the main image and
    // the interpreter without any relocation pass; ld.so does the rest.
    // Binaries without PT_INTERP are mapped by the plain loader (static ELF).
    let mut interp_path = [0u8; 256];
    let has_interp =
        ffi::elf_get_interp_path(path, interp_path.as_mut_ptr(), 256) > 0;
    let mut interp_info = ffi::InterpInfo {
        main_entry:  0,
        main_base:   0,
        main_phdr:   0,
        main_phnum:  0,
        interp_base: 0,
    };

    // Check execute permission before loading
    {
        let exec_node = ffi::vfs_walk_path(unsafe { *ffi::vfs_root.get() }, path);
        if exec_node.is_null() || ffi::vfs_check_perm(exec_node, 0x01) < 0 {
            ffi::vmm_free_address_space(new_pd);
            for page in new_ustack_pages {
                ffi::free_page(page);
            }
            return -1;
        }
    }

    let entry = if has_interp {
        ffi::load_elf_interp(
            path,
            interp_path.as_ptr(),
            new_pd,
            &raw mut (*p).mm,
            &raw mut interp_info,
        )
    } else {
        ffi::load_elf(path, new_pd, &raw mut (*p).mm)
    };
    if entry.is_null() {
        ffi::vmm_free_address_space(new_pd);
        for page in new_ustack_pages {
            ffi::free_page(page);
        }
        return -1;
    }

    // Load symbol table for crash traces
    ffi::elf_load_exec_symtab(path, p as *mut c_void);

    if TRACE_PROC_LOGS {
        let mut hbuf = [0u8; 12];
        ffi::printk(b"[EXEC] load_elf entry=0x\0".as_ptr());
        ffi::hex_to_ascii(entry as u32, hbuf.as_mut_ptr());
        ffi::printk(hbuf.as_ptr());
        ffi::printk(b" pid=\0".as_ptr());
        ffi::itoa((*t).pid as i32, hbuf.as_mut_ptr());
        ffi::printk(hbuf.as_ptr());
        ffi::printk(b"\n\0".as_ptr());

        let pdi = (entry as usize) >> 22;
        let pti = ((entry as usize) >> 12) & 0x3FF;
        let pde = unsafe { *new_pd.add(pdi) };
        ffi::printk(b"[EXEC] PDE for entry pdi=\0".as_ptr());
        ffi::itoa(pdi as i32, hbuf.as_mut_ptr());
        ffi::printk(hbuf.as_ptr());
        ffi::printk(b": 0x\0".as_ptr());
        ffi::hex_to_ascii(pde, hbuf.as_mut_ptr());
        ffi::printk(hbuf.as_ptr());
        if pde == 0 || pde & PAGE_PRESENT == 0 {
            ffi::printk(b" ABSENT (no exec mapping)\n\0".as_ptr());
        } else {
            ffi::printk(b" OK\n\0".as_ptr());
            let pt = (pde & !0xFFFu32) as *mut u32;
            let pte = unsafe { *pt.add(pti) };
            ffi::printk(b"[EXEC] PTE for entry pdi=\0".as_ptr());
            ffi::itoa(pdi as i32, hbuf.as_mut_ptr());
            ffi::printk(hbuf.as_ptr());
            ffi::printk(b" pti=\0".as_ptr());
            ffi::itoa(pti as i32, hbuf.as_mut_ptr());
            ffi::printk(hbuf.as_ptr());
            ffi::printk(b": 0x\0".as_ptr());
            ffi::hex_to_ascii(pte, hbuf.as_mut_ptr());
            ffi::printk(hbuf.as_ptr());
            if pte == 0 || pte & PAGE_PRESENT == 0 {
                ffi::printk(b" ABSENT (page not mapped)\n\0".as_ptr());
            } else {
                ffi::printk(b" OK\n\0".as_ptr());
            }
        }
    }

    irq_spinlock_acquire(&raw mut SCHEDULER_LOCK);

    (*p).ustack_phys = new_ustack_pages[0];
    (*p).ustack_phys_extra = [new_ustack_pages[1], new_ustack_pages[2], new_ustack_pages[3]];
    map_user_stack_in_pd(new_pd, &*p);

    for pi in 0..USER_STACK_PAGES as usize {
        let us = new_ustack_pages[pi] as *mut u8;
        ffi::memory_set(us as *mut c_void, 0, PAGE_SIZE as usize);
    }

    {
        let file = ffi::vfs_walk_path(unsafe { *ffi::vfs_root.get() }, path);
        if !file.is_null() {
            let brk = ffi::elf_get_brk_start(file);
            (*p).brk_start   = brk;
            (*p).brk_current = brk;
        }
    }

    map_sigreturn_trampoline_on_pd(t, new_pd);
    ffi::vmm_sync_kernel_mmio_mappings(new_pd);

    let esp0 = (*p).stack_base as u32 + KERNEL_STACK_SIZE as u32;
    unsafe { (*ffi::tss_entry.get()).esp0 = esp0; }
    ffi::syscall_set_esp0(esp0);

    let ustack_top = (*p).ustack_virt + USER_STACK_BYTES;
    let mut sp     = ustack_top - 4;

    let mut argv_vaddrs: [u32; 256] = [0; 256];
    let mut envp_vaddrs: [u32; 256] = [0; 256];

    let argc = match copy_strings_to_ustack(argv, EXEC_MAX_ARGS, &*p, &mut sp, &mut argv_vaddrs) {
        Some(n) => n,
        None => {
            ffi::printk(b"[EXEC] abort: argv copy / stack overflow\n\0".as_ptr());
            (*p).ustack_phys = old_ustack_pages[0];
            (*p).ustack_phys_extra = [old_ustack_pages[1], old_ustack_pages[2], old_ustack_pages[3]];
            ffi::vmm_free_address_space(new_pd);
            irq_spinlock_release(&raw mut SCHEDULER_LOCK);
            return -1;
        }
    };
    let envc = match copy_strings_to_ustack(envp, EXEC_MAX_ENVS, &*p, &mut sp, &mut envp_vaddrs) {
        Some(n) => n,
        None => {
            ffi::printk(b"[EXEC] abort: envp copy / stack overflow\n\0".as_ptr());
            (*p).ustack_phys = old_ustack_pages[0];
            (*p).ustack_phys_extra = [old_ustack_pages[1], old_ustack_pages[2], old_ustack_pages[3]];
            ffi::vmm_free_address_space(new_pd);
            irq_spinlock_release(&raw mut SCHEDULER_LOCK);
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
        sp -= 4; ustack_write_u32(&*p, sp, 0); // auxv terminator (val)
        sp -= 4; ustack_write_u32(&*p, sp, 0); // auxv terminator (tag)
        for i in (0..auxc).rev() {
            sp -= 4; ustack_write_u32(&*p, sp, auxv[i].1);
            sp -= 4; ustack_write_u32(&*p, sp, auxv[i].0);
        }
    }

    // The pointer-layout block below stays identical for both exec paths.
    let ptr_overhead = (argc as u32 + envc as u32 + 5) * 4;
    if sp < (*p).ustack_virt + ptr_overhead {
        ffi::printk(b"[EXEC] abort: stack layout preflight failed\n\0".as_ptr());
        (*p).ustack_phys = old_ustack_pages[0];
        (*p).ustack_phys_extra = [old_ustack_pages[1], old_ustack_pages[2], old_ustack_pages[3]];
        ffi::vmm_free_address_space(new_pd);
        irq_spinlock_release(&raw mut SCHEDULER_LOCK);
        return -1;
    }

    sp -= 4;
    ustack_write_u32(&*p, sp, 0);
    for i in (0..envc).rev() {
        sp -= 4;
        ustack_write_u32(&*p, sp, envp_vaddrs[i]);
    }
    let envp_arr = sp;

    sp -= 4;
    ustack_write_u32(&*p, sp, 0);
    for i in (0..argc).rev() {
        sp -= 4;
        ustack_write_u32(&*p, sp, argv_vaddrs[i]);
    }
    let argv_arr = sp;

    sp -= 4; ustack_write_u32(&*p, sp, envp_arr);
    sp -= 4; ustack_write_u32(&*p, sp, argv_arr);
    sp -= 4; ustack_write_u32(&*p, sp, argc as u32);

    (*p).pending_signals = 0;
    for i in 0..NSIG { (*p).signal_handlers[i] = SIG_DFL; }

    ffi::shm_detach_all((*t).pid, new_pd);
    for i in 0..TASK_SHM_MAX {
        (*p).shm_attachments[i].shm_id    = 0;
        (*p).shm_attachments[i].shm_vaddr = 0;
    }

    ffi::mmap_table_init((*p).mmap_table);

    for i in 3..MAX_FD {
        let ft = (*(*p).fds).fd_table[i];
        if !ft.is_null() && (*(*p).fds).fd_cloexec[i] != 0 {
            ffi::file_unref(ft as *mut c_void);
            (*(*p).fds).fd_table[i]   = ptr::null_mut();
            (*(*p).fds).fd_offset[i]  = 0;
            (*(*p).fds).fd_flags[i]   = 0;
            (*(*p).fds).fd_cloexec[i] = 0;
        }
    }

    map_user_stack_in_pd(new_pd, &*p);

    let old_pd = (*t).page_directory;
    (*t).page_directory = new_pd;
    ffi::switch_paging(new_pd);
    if !old_pd.is_null() {
        ffi::vmm_free_address_space(old_pd);
    }
    ffi::vmm_sync_kernel_mmio_mappings(new_pd);

    unsafe {
        core::arch::asm!(
            "invlpg [{}]",
            in(reg) entry as u32,
            options(nostack),
        );
    }

    if TRACE_PROC_LOGS {
        let mut hbuf = [0u8; 12];
        let ent = entry as u32;
        let pdi = (ent as usize) >> 22;
        let pti = ((ent as usize) >> 12) & 0x3FF;
        let pde = unsafe { *new_pd.add(pdi) };
        let pte = if pde & PAGE_PRESENT == 0 {
            0u32
        } else {
            let pt = (pde & !0xFFFu32) as *const u32;
            unsafe { *pt.add(pti) }
        };
        let user_ok = (pde & PAGE_PRESENT != 0)
            && (pde & PAGE_USER != 0)
            && (pte & PAGE_PRESENT != 0)
            && (pte & PAGE_USER != 0);
        if !user_ok {
            ffi::printk(b"[EXEC] POST-FREE entry map not user: PDE=0x\0".as_ptr());
            ffi::hex_to_ascii(pde, hbuf.as_mut_ptr());
            ffi::printk(hbuf.as_ptr());
            ffi::printk(b" PTE=0x\0".as_ptr());
            ffi::hex_to_ascii(pte, hbuf.as_mut_ptr());
            ffi::printk(hbuf.as_ptr());
            ffi::printk(b"\n\0".as_ptr());
        }
    }

    if TRACE_PROC_LOGS {
        ffi::printk(b"[EXEC] committed: new AS; iretd next\n\0".as_ptr());
    }

    irq_spinlock_release(&raw mut SCHEDULER_LOCK);

    unsafe { *ffi::terminal_fg_pid.get() = (*t).pid; }

    let pd_val = new_pd as u32;
    let entry_u = entry as u32;
    let sp_u = sp;

    if TRACE_PROC_LOGS {
        let mut hbuf = [0u8; 12];
        let ustack_hi = (*p).ustack_virt.wrapping_add(USER_STACK_BYTES);
        ffi::printk(b"[EXEC] iretd esp=0x\0".as_ptr());
        ffi::hex_to_ascii(sp_u, hbuf.as_mut_ptr());
        ffi::printk(hbuf.as_ptr());
        ffi::printk(b" ustack_page=[0x\0".as_ptr());
        ffi::hex_to_ascii((*p).ustack_virt, hbuf.as_mut_ptr());
        ffi::printk(hbuf.as_ptr());
        ffi::printk(b"..0x\0".as_ptr());
        ffi::hex_to_ascii(ustack_hi, hbuf.as_mut_ptr());
        ffi::printk(hbuf.as_ptr());
        ffi::printk(b") e_entry=0x\0".as_ptr());
        ffi::hex_to_ascii(entry_u, hbuf.as_mut_ptr());
        ffi::printk(hbuf.as_ptr());
        ffi::printk(b"\n\0".as_ptr());

        const ADDR_HINT: u32 = 0x0800_0EAD;
        if sp_u.abs_diff(ADDR_HINT) < 0x1000 {
            ffi::printk(
                b"[EXEC] WARNING: esp within 4KiB of 0x08000EAD (stack vs code?)\n\0".as_ptr(),
            );
        }
        if sp_u >= 0x0800_0000 && sp_u < 0x0B00_0000 {
            ffi::printk(
                b"[EXEC] WARNING: esp in low 0x0800_0000..0x0B00_0000 (expect ~0xBFF...)\n\0".as_ptr(),
            );
        }
        if sp_u < (*p).ustack_virt || sp_u >= ustack_hi {
            ffi::printk(b"[EXEC] WARNING: esp outside mapped ustack region\n\0".as_ptr());
        }
    }

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
    unsafe {
        let mut count = 0usize;
        for i in 0..max {
            let s = *src.add(i);
            if s.is_null() { break; }

            let mut len = 0usize;
            while len < EXEC_MAX_STRLEN && *s.add(len) != 0 { len += 1; }
            if len >= EXEC_MAX_STRLEN { return None; }

            if *sp < p.ustack_virt + (len + 1 + count * 4) as u32 { return None; }

            *sp = (*sp).wrapping_sub(len as u32 + 1);
            for j in 0..=len {
                *(ustack_kernel_byte_mut(p, *sp + j as u32)) = *s.add(j);
            }
            vaddrs[i] = *sp;
            count += 1;
        }
        Some(count)
    }
}
