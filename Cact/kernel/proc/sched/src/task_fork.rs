//! Process forking (`task_fork`), exit/wait machinery, and zombie reaping.

use core::ffi::c_void;
use core::ptr;
use crate::ffi::{self, ContextFrame, MmapTable, VfsNode, PAGE_PRESENT, PAGE_RW, PAGE_SIZE, PAGE_USER};
use crate::mlfq;
use crate::sync::{irq_spinlock_acquire, irq_spinlock_release};
use crate::task::{
    current_task, find_task_by_pid, next_pid, task_list_add, task_list_head,
    task_list_remove, task_setup_sigreturn, ustack_phys_by_idx, ProcMeta, TaskStruct,
    TaskState, TRACE_PROC_LOGS, KERNEL_STACK_SIZE, MAX_FD, SCHEDULER_LOCK,
    TASK_SHM_MAX, USER_STACK_PAGES,
};

#[no_mangle]
pub unsafe extern "C" fn task_fork(regs: *mut ContextFrame) -> *mut TaskStruct {
    irq_spinlock_acquire(&raw mut SCHEDULER_LOCK);

    let parent = current_task;
    if parent.is_null() {
        irq_spinlock_release(&raw mut SCHEDULER_LOCK);
        return ptr::null_mut();
    }

    let child_pd = ffi::vmm_create_address_space();
    if child_pd.is_null() {
        irq_spinlock_release(&raw mut SCHEDULER_LOCK);
        return ptr::null_mut();
    }

    let child = ffi::kmalloc(core::mem::size_of::<TaskStruct>()) as *mut TaskStruct;
    if child.is_null() {
        ffi::vmm_free_address_space(child_pd);
        irq_spinlock_release(&raw mut SCHEDULER_LOCK);
        return ptr::null_mut();
    }

    let child_p = ffi::kmalloc(core::mem::size_of::<ProcMeta>()) as *mut ProcMeta;
    if child_p.is_null() {
        ffi::kfree(child as *mut c_void);
        ffi::vmm_free_address_space(child_pd);
        irq_spinlock_release(&raw mut SCHEDULER_LOCK);
        return ptr::null_mut();
    }

    let kstack = ffi::kalloc() as *mut u32;
    if kstack.is_null() {
        ffi::kfree(child_p as *mut c_void);
        ffi::kfree(child as *mut c_void);
        ffi::vmm_free_address_space(child_pd);
        irq_spinlock_release(&raw mut SCHEDULER_LOCK);
        return ptr::null_mut();
    }

    let mut ustack_pages: [*mut c_void; USER_STACK_PAGES as usize] =
        [ptr::null_mut(); USER_STACK_PAGES as usize];
    for i in 0..USER_STACK_PAGES as usize {
        let page = ffi::kalloc() as *mut c_void;
        if page.is_null() {
            for j in 0..i {
                ffi::free_page(ustack_pages[j]);
            }
            ffi::free_page(kstack as *mut c_void);
            ffi::kfree(child_p as *mut c_void);
            ffi::kfree(child as *mut c_void);
            ffi::vmm_free_address_space(child_pd);
            irq_spinlock_release(&raw mut SCHEDULER_LOCK);
            return ptr::null_mut();
        }
        ustack_pages[i] = page;
    }

    ffi::memory_copy(child as *mut c_void, parent as *const c_void,
                     core::mem::size_of::<TaskStruct>());

    let parent_p = (*parent).proc;
    ffi::memory_copy(child_p as *mut c_void, parent_p as *const c_void,
                     core::mem::size_of::<ProcMeta>());

    (*child).pid            = next_pid;
    next_pid               += 1;
    (*child).state          = TaskState::Ready;
    (*child).page_directory = child_pd;
    (*child).proc           = child_p;
    (*child).next           = ptr::null_mut();
    (*child).queue_next     = ptr::null_mut();

    (*child_p).stack_base     = kstack as *mut c_void;
    (*child_p).ustack_phys    = ustack_pages[0];
    (*child_p).ustack_phys_extra = [ustack_pages[1], ustack_pages[2], ustack_pages[3]];
    (*child_p).parent_pid     = (*parent).pid;
    (*child_p).exit_code      = 0;
    (*child_p).wait_for_pid   = 0;
    (*child_p).sleep_until    = 0;
    (*child_p).pending_signals = 0;
    (*child_p).wait_next      = ptr::null_mut();

    ffi::proc_tracker_init(&raw mut (*child_p).mm);
    (*child_p).mm.page_dir = child_pd;

    let child_fds = ffi::kmalloc(core::mem::size_of::<ffi::TaskFdTable>()) as *mut ffi::TaskFdTable;
    if child_fds.is_null() {
        for page in ustack_pages {
            ffi::free_page(page);
        }
        ffi::free_page(kstack as *mut c_void);
        ffi::kfree(child_p as *mut c_void);
        ffi::kfree(child as *mut c_void);
        ffi::vmm_free_address_space(child_pd);
        irq_spinlock_release(&raw mut SCHEDULER_LOCK);
        return ptr::null_mut();
    }
    ffi::memory_copy(child_fds as *mut c_void, (*parent_p).fds as *const c_void,
                     core::mem::size_of::<ffi::TaskFdTable>());

    for i in 0..MAX_FD {
        let ft = (*child_fds).fd_table[i];
        if !ft.is_null() {
            ffi::file_ref(ft as *mut c_void);
        }
    }

    (*child_p).fds = child_fds;

    let child_mmap = ffi::kmalloc(core::mem::size_of::<MmapTable>()) as *mut MmapTable;
    if child_mmap.is_null() {
        ffi::kfree(child_fds as *mut c_void);
        (*child_p).fds = ptr::null_mut();
        for page in ustack_pages {
            ffi::free_page(page);
        }
        ffi::free_page(kstack as *mut c_void);
        ffi::kfree(child_p as *mut c_void);
        ffi::kfree(child as *mut c_void);
        ffi::vmm_free_address_space(child_pd);
        irq_spinlock_release(&raw mut SCHEDULER_LOCK);
        return ptr::null_mut();
    }
    ffi::mmap_table_init(child_mmap);
    (*child_p).mmap_table = child_mmap;

    if !(*parent).page_directory.is_null() {
        ffi::vmm_fork_address_space((*parent).page_directory, child_pd);
    }

    ffi::mmap_table_clone(
        (*parent_p).mmap_table,
        (*child_p).mmap_table,
        (*parent).page_directory,
        child_pd,
    );

    // The kernel MMIO mappings (framebuffer, xHCI BARs...) must be present in
    // the child pd; fork only deep-copies the user half.
    ffi::vmm_sync_kernel_mmio_mappings(child_pd);

    for i in 0..USER_STACK_PAGES as usize {
        let vaddr = (*child_p).ustack_virt.wrapping_add((i as u32).wrapping_mul(PAGE_SIZE));
        let cphys = ustack_phys_by_idx(&*child_p, i) as u32;
        ffi::vmm_map(child_pd, vaddr, cphys, PAGE_USER | PAGE_RW | PAGE_PRESENT);
        ffi::memory_copy(
            ustack_phys_by_idx(&*child_p, i),
            ustack_phys_by_idx(&*parent_p, i) as *const c_void,
            PAGE_SIZE as usize,
        );
    }

    for i in 0..MAX_FD {
        let ft = (*(*child_p).fds).fd_table[i];
        if !ft.is_null() {
            let node = unsafe { *(ft as *const *mut VfsNode) };
            if !node.is_null() {
                ffi::open_vfs(node);
            }
        }
    }

    for i in 0..TASK_SHM_MAX {
        (*child_p).shm_attachments[i].shm_id    = 0;
        (*child_p).shm_attachments[i].shm_vaddr = 0;
    }

    let stack_top_ptr = (kstack as usize + KERNEL_STACK_SIZE) as *mut u32;
    let mut esp_ptr = stack_top_ptr;

    esp_ptr = esp_ptr.sub(1); *esp_ptr = (*regs).ss;
    esp_ptr = esp_ptr.sub(1); *esp_ptr = (*regs).useresp;
    esp_ptr = esp_ptr.sub(1); *esp_ptr = (*regs).eflags;
    esp_ptr = esp_ptr.sub(1); *esp_ptr = (*regs).cs;
    esp_ptr = esp_ptr.sub(1); *esp_ptr = (*regs).eip;
    esp_ptr = esp_ptr.sub(1); *esp_ptr = 0;
    esp_ptr = esp_ptr.sub(1); *esp_ptr = (*regs).ecx;
    esp_ptr = esp_ptr.sub(1); *esp_ptr = (*regs).edx;
    esp_ptr = esp_ptr.sub(1); *esp_ptr = (*regs).ebx;
    esp_ptr = esp_ptr.sub(1); *esp_ptr = 0;
    esp_ptr = esp_ptr.sub(1); *esp_ptr = (*regs).ebp;
    esp_ptr = esp_ptr.sub(1); *esp_ptr = (*regs).esi;
    esp_ptr = esp_ptr.sub(1); *esp_ptr = (*regs).edi;
    esp_ptr = esp_ptr.sub(1); *esp_ptr = (*regs).ds;
    esp_ptr = esp_ptr.sub(1); *esp_ptr = (*regs).es;

    esp_ptr = esp_ptr.sub(1); *esp_ptr = ffi::fork_task_trampoline as *const () as u32;
    esp_ptr = esp_ptr.sub(1); *esp_ptr = 0;
    esp_ptr = esp_ptr.sub(1); *esp_ptr = 0;
    esp_ptr = esp_ptr.sub(1); *esp_ptr = 0;
    esp_ptr = esp_ptr.sub(1); *esp_ptr = 0;

    (*child).esp = esp_ptr as u32;

    task_setup_sigreturn(child);

    task_list_add(child);
    mlfq::mlfq_enqueue_locked(child, (*child).priority);

    if TRACE_PROC_LOGS {
        let mut buf = [0u8; 12];
        ffi::printk(b"[FORK] parent=\0".as_ptr());
        ffi::itoa((*parent).pid as i32, buf.as_mut_ptr());
        ffi::printk(buf.as_ptr());
        ffi::printk(b" child=\0".as_ptr());
        ffi::itoa((*child).pid as i32, buf.as_mut_ptr());
        ffi::printk(buf.as_ptr());
        ffi::printk(b" eip=0x\0".as_ptr());
        ffi::hex_to_ascii((*regs).eip, buf.as_mut_ptr());
        ffi::printk(buf.as_ptr());
        ffi::printk(b" uesp=0x\0".as_ptr());
        ffi::hex_to_ascii((*regs).useresp, buf.as_mut_ptr());
        ffi::printk(buf.as_ptr());
        ffi::printk(b" cs=0x\0".as_ptr());
        ffi::hex_to_ascii((*regs).cs, buf.as_mut_ptr());
        ffi::printk(buf.as_ptr());
        ffi::printk(b" ss=0x\0".as_ptr());
        ffi::hex_to_ascii((*regs).ss, buf.as_mut_ptr());
        ffi::printk(buf.as_ptr());
        ffi::printk(b" child_esp=0x\0".as_ptr());
        ffi::hex_to_ascii((*child).esp, buf.as_mut_ptr());
        ffi::printk(buf.as_ptr());
        ffi::printk(b"\n\0".as_ptr());
    }

    irq_spinlock_release(&raw mut SCHEDULER_LOCK);

    child
}

#[no_mangle]
pub unsafe extern "C" fn sched_task_exit(exit_code: i32) {
    let t = current_task;
    if t.is_null() { return; }

    let p = (*t).proc;

    if TRACE_PROC_LOGS {
        let mut buf = [0u8; 12];
        ffi::printk(b"[EXIT] pid=\0".as_ptr());
        ffi::itoa((*t).pid as i32, buf.as_mut_ptr());
        ffi::printk(buf.as_ptr());
        ffi::printk(b" code=\0".as_ptr());
        ffi::itoa(exit_code, buf.as_mut_ptr());
        ffi::printk(buf.as_ptr());
        ffi::printk(b" parent=\0".as_ptr());
        ffi::itoa((*p).parent_pid as i32, buf.as_mut_ptr());
        ffi::printk(buf.as_ptr());
        ffi::printk(b"\n\0".as_ptr());
    }

    irq_spinlock_acquire(&raw mut SCHEDULER_LOCK);

    (*p).exit_code = exit_code;
    (*t).state = TaskState::Zombie;

    let my_pid = (*t).pid;
    let mut child = task_list_head;
    while !child.is_null() {
        let next_child = (*child).next;
        if (*(*child).proc).parent_pid == my_pid {
            (*(*child).proc).parent_pid = 0;
        }
        child = next_child;
    }

    irq_spinlock_release(&raw mut SCHEDULER_LOCK);

    crate::mlfq::schedule();
}

#[no_mangle]
pub unsafe extern "C" fn sched_waitpid(target_pid: i32, status: *mut i32) -> i32 {
    let cur = current_task;
    if cur.is_null() { return -1; }

    loop {
        irq_spinlock_acquire(&raw mut SCHEDULER_LOCK);

        let mut t = task_list_head;
        let mut found_child = false;

        while !t.is_null() {
            if (*(*t).proc).parent_pid == (*cur).pid
                && (target_pid <= 0 || (*t).pid == target_pid as u32)
            {
                if matches!((*t).state, TaskState::Zombie) {
                    let child_pid  = (*t).pid;
                    let child_exit = (*(*t).proc).exit_code;
                    task_list_remove(t);
                    let to_free = t;
                    irq_spinlock_release(&raw mut SCHEDULER_LOCK);

                    if TRACE_PROC_LOGS {
                        let mut buf = [0u8; 12];
                        ffi::printk(b"[WAIT] reaped pid=\0".as_ptr());
                        ffi::itoa(child_pid as i32, buf.as_mut_ptr());
                        ffi::printk(buf.as_ptr());
                        ffi::printk(b" exit=\0".as_ptr());
                        ffi::itoa(child_exit, buf.as_mut_ptr());
                        ffi::printk(buf.as_ptr());
                        ffi::printk(b"\n\0".as_ptr());
                    }

                    reap_task_free(to_free);

                    if !status.is_null() {
                        *status = child_exit;
                    }
                    return child_pid as i32;
                }
                found_child = true;
            }
            t = (*t).next;
        }

        if !found_child {
            if TRACE_PROC_LOGS {
                ffi::printk(b"[WAIT] no children found!\n\0".as_ptr());
            }
            irq_spinlock_release(&raw mut SCHEDULER_LOCK);
            return -1;
        }

        if TRACE_PROC_LOGS {
            ffi::printk(b"[WAIT] blocking, child alive\n\0".as_ptr());
        }
        (*cur).state = TaskState::Waiting;
        irq_spinlock_release(&raw mut SCHEDULER_LOCK);
        crate::mlfq::schedule();
    }
}

fn reap_task_free(t: *mut TaskStruct) {
    if t.is_null() {
        return;
    }
    unsafe {
        let p = (*t).proc;
        if p.is_null() {
            return;
        }

        if !(*p).fds.is_null() {
            for j in 0..MAX_FD {
                let ft = (*(*p).fds).fd_table[j];
                if !ft.is_null() {
                    ffi::file_unref(ft as *mut c_void);
                }
            }
            ffi::kfree((*p).fds as *mut c_void);
            (*p).fds = ptr::null_mut();
        }

        if !(*p).mmap_table.is_null() {
            let mt = (*p).mmap_table;
            let pd = (*t).page_directory;
            if !pd.is_null() {
                ffi::mmap_table_free(mt, pd);
            }
            ffi::kfree(mt as *mut c_void);
            (*p).mmap_table = ptr::null_mut();
        }

        ffi::shm_detach_all((*t).pid, (*t).page_directory);
        ffi::proc_free_pages(&raw mut (*p).mm);

        (*t).page_directory = ptr::null_mut();
        (*p).ustack_phys    = ptr::null_mut();
        (*p).ustack_phys_extra = [ptr::null_mut(); 3];

        if !(*p).stack_base.is_null() {
            ffi::free_page((*p).stack_base);
        }

        ffi::kfree(p as *mut c_void);
        ffi::kfree(t as *mut c_void);
    }
}

#[no_mangle]
pub unsafe extern "C" fn task_reap() {
    let mut to_reap: [*mut TaskStruct; 64] = [ptr::null_mut(); 64];
    let mut count = 0usize;

    irq_spinlock_acquire(&raw mut SCHEDULER_LOCK);
    let mut cur = task_list_head;
    while !cur.is_null() && count < 64 {
        let next = (*cur).next;
        if matches!((*cur).state, TaskState::Zombie) {
            let parent_pid_val = (*(*cur).proc).parent_pid;
            let reapable = parent_pid_val == 0
                || find_task_by_pid(parent_pid_val).is_null();
            if reapable {
                task_list_remove(cur);
                to_reap[count] = cur;
                count += 1;
            }
        }
        cur = next;
    }
    irq_spinlock_release(&raw mut SCHEDULER_LOCK);

    for i in 0..count {
        reap_task_free(to_reap[i]);
    }
}
