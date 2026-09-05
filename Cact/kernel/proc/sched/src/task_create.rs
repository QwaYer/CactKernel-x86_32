//! Task creation: kernel tasks, plain user tasks, and ELF-backed processes
//! (static and dynamically linked).

use core::ffi::c_void;
use core::ptr;
use crate::ffi::{self, ProcPageTracker};
use crate::mlfq;
use crate::sync::{irq_spinlock_acquire, irq_spinlock_release};
use crate::task::{
    calc_highest_mapped_va, current_task, free_user_stack_pages, map_user_stack_in_pd,
    next_pid, push_empty_args, task_list_add, task_setup_sigreturn, task_zero_init,
    ustack_write_u32, ProcMeta, TaskStruct, SCHEDULER_LOCK, KERNEL_BASE, KERNEL_STACK_SIZE,
    USER_CODE_SEL, USER_DATA_SEL, USER_STACK_BYTES, USER_STACK_PAGES,
};

#[no_mangle]
pub unsafe extern "C" fn create_task(entry_point: *const c_void) -> *mut TaskStruct {
    let t = ffi::kmalloc(core::mem::size_of::<TaskStruct>()) as *mut TaskStruct;
    if t.is_null() { return ptr::null_mut(); }

    let p = ffi::kmalloc(core::mem::size_of::<ProcMeta>()) as *mut ProcMeta;
    if p.is_null() { ffi::kfree(t as *mut c_void); return ptr::null_mut(); }

    let stack = ffi::kalloc() as *mut u32;
    if stack.is_null() { ffi::kfree(p as *mut c_void); ffi::kfree(t as *mut c_void); return ptr::null_mut(); }

    if !task_zero_init(t, p) {
        ffi::free_page(stack as *mut c_void);
        ffi::kfree(p as *mut c_void);
        ffi::kfree(t as *mut c_void);
        return ptr::null_mut();
    }

    let stack_top = (stack as usize + KERNEL_STACK_SIZE) as *mut u32;
    let mut esp = stack_top;

    esp = esp.sub(1); *esp = entry_point as u32;
    esp = esp.sub(1); *esp = ffi::kernel_task_trampoline as *const () as u32;
    esp = esp.sub(1); *esp = 0;
    esp = esp.sub(1); *esp = 0;
    esp = esp.sub(1); *esp = 0;
    esp = esp.sub(1); *esp = 0;

    (*t).esp           = esp as u32;
    (*t).pid           = next_pid;
    next_pid          += 1;
    (*t).is_kernel     = 1;
    (*t).page_directory = ptr::null_mut();
    (*p).stack_base    = stack as *mut c_void;

    irq_spinlock_acquire(&raw mut SCHEDULER_LOCK);
    task_list_add(t);
    mlfq::mlfq_enqueue_locked(t, (*t).priority);
    irq_spinlock_release(&raw mut SCHEDULER_LOCK);

    t
}

fn create_user_task_internal(entry_point: *const c_void, add_to_list: bool) -> *mut TaskStruct {
    unsafe {
    let t = ffi::kmalloc(core::mem::size_of::<TaskStruct>()) as *mut TaskStruct;
    if t.is_null() { return ptr::null_mut(); }

    let p = ffi::kmalloc(core::mem::size_of::<ProcMeta>()) as *mut ProcMeta;
    if p.is_null() { ffi::kfree(t as *mut c_void); return ptr::null_mut(); }

    let kstack = ffi::kalloc() as *mut u32;
    if kstack.is_null() { ffi::kfree(p as *mut c_void); ffi::kfree(t as *mut c_void); return ptr::null_mut(); }

    let mut ustack_pages: [*mut c_void; USER_STACK_PAGES as usize] =
        [ptr::null_mut(); USER_STACK_PAGES as usize];
    for i in 0..USER_STACK_PAGES as usize {
        let page = ffi::kalloc() as *mut c_void;
        if page.is_null() {
            for j in 0..i {
                ffi::free_page(ustack_pages[j]);
            }
            ffi::free_page(kstack as *mut c_void);
            ffi::kfree(p as *mut c_void);
            ffi::kfree(t as *mut c_void);
            return ptr::null_mut();
        }
        ustack_pages[i] = page;
    }

    if !task_zero_init(t, p) {
        for page in ustack_pages {
            ffi::free_page(page);
        }
        ffi::free_page(kstack as *mut c_void);
        ffi::kfree(p as *mut c_void);
        ffi::kfree(t as *mut c_void);
        return ptr::null_mut();
    }

    let ustack_virt: u32 = KERNEL_BASE - USER_STACK_BYTES;

    let stack_top = (kstack as usize + KERNEL_STACK_SIZE) as *mut u32;
    let mut esp = stack_top;

    esp = esp.sub(1); *esp = USER_DATA_SEL;
    esp = esp.sub(1); *esp = ustack_virt + USER_STACK_BYTES - 4;
    esp = esp.sub(1); *esp = 0x0000_0202;
    esp = esp.sub(1); *esp = USER_CODE_SEL;
    esp = esp.sub(1); *esp = entry_point as u32;
    esp = esp.sub(1); *esp = ffi::user_task_trampoline as *const () as u32;
    esp = esp.sub(1); *esp = 0;
    esp = esp.sub(1); *esp = 0;
    esp = esp.sub(1); *esp = 0;
    esp = esp.sub(1); *esp = 0;

    (*t).esp          = esp as u32;
    (*p).stack_base   = kstack as *mut c_void;
    (*p).ustack_phys  = ustack_pages[0];
    (*p).ustack_phys_extra = [ustack_pages[1], ustack_pages[2], ustack_pages[3]];
    (*p).ustack_virt  = ustack_virt;
    (*t).pid          = next_pid;
    next_pid         += 1;
    (*t).is_kernel    = 0;
    (*p).parent_pid   = if !current_task.is_null() { (*current_task).pid } else { 0 };
    (*p).uid  = if !current_task.is_null() { (*(*current_task).proc).uid  } else { 0 };
    (*p).gid  = if !current_task.is_null() { (*(*current_task).proc).gid  } else { 0 };
    (*p).euid = if !current_task.is_null() { (*(*current_task).proc).euid } else { 0 };
    (*p).egid = if !current_task.is_null() { (*(*current_task).proc).egid } else { 0 };

    if add_to_list {
        irq_spinlock_acquire(&raw mut SCHEDULER_LOCK);
        task_list_add(t);
        mlfq::mlfq_enqueue_locked(t, (*t).priority);
        irq_spinlock_release(&raw mut SCHEDULER_LOCK);
    }

    t
    }
}

#[no_mangle]
pub unsafe extern "C" fn create_user_task(entry_point: *const c_void) -> *mut TaskStruct {
    create_user_task_internal(entry_point, true)
}

#[no_mangle]
pub unsafe extern "C" fn create_task_with_entry(
    entry:   *const c_void,
    pd:      *mut u32,
    tracker: *mut ProcPageTracker,
) -> *mut TaskStruct {
    let t = create_user_task_internal(entry, false);
    if t.is_null() { return ptr::null_mut(); }

    let p = (*t).proc;
    ffi::memory_copy(&raw mut (*p).mm as *mut c_void, tracker as *const c_void,
                     core::mem::size_of::<ProcPageTracker>());
    (*t).page_directory = pd;

    let stk = (*t).esp as *mut u32;
    *stk.add(5) = entry as u32;

    map_user_stack_in_pd(pd, &*p);

    let highest = calc_highest_mapped_va(pd);
    (*p).brk_start   = highest;
    (*p).brk_current = highest;

    let ustack_top = (*p).ustack_virt + USER_STACK_BYTES;
    let mut sp = ustack_top - 4;
    push_empty_args(&*p, &mut sp);
    *stk.add(8) = sp;

    task_setup_sigreturn(t);

    irq_spinlock_acquire(&raw mut SCHEDULER_LOCK);
    task_list_add(t);
    mlfq::mlfq_enqueue_locked(t, (*t).priority);
    irq_spinlock_release(&raw mut SCHEDULER_LOCK);

    t
}

#[no_mangle]
pub unsafe extern "C" fn create_elf_task(path: *const u8) -> *mut TaskStruct {
    let pd = ffi::vmm_create_address_space();
    if pd.is_null() { return ptr::null_mut(); }

    let t = create_user_task_internal(ptr::null(), false);
    if t.is_null() { ffi::vmm_free_address_space(pd); return ptr::null_mut(); }

    let p = (*t).proc;
    ffi::proc_tracker_init(&raw mut (*p).mm);

    // PT_INTERP handoff (userspace ld.so) — same protocol as task_exec.
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

    let entry = if has_interp {
        ffi::load_elf_interp(path, interp_path.as_ptr(), pd,
                             &raw mut (*p).mm, &raw mut interp_info)
    } else {
        ffi::load_elf(path, pd, &raw mut (*p).mm)
    };
    if entry.is_null() {
        ffi::free_page((*p).stack_base);
        free_user_stack_pages(&mut *p);
        ffi::kfree(p as *mut c_void);
        ffi::kfree(t as *mut c_void);
        ffi::vmm_free_address_space(pd);
        return ptr::null_mut();
    }

    (*t).page_directory = pd;

    // Load symbol table for crash traces
    ffi::elf_load_exec_symtab(path, p as *mut c_void);

    let stk = (*t).esp as *mut u32;
    *stk.add(5) = entry as u32;

    map_user_stack_in_pd(pd, &*p);

    let highest = calc_highest_mapped_va(pd);
    if has_interp {
        // With the interpreter mapped too, calc_highest lands above ld.so;
        // the brk must start at the end of the *main* image instead.
        let main_node = ffi::vfs_walk_path(unsafe { *ffi::vfs_root.get() }, path);
        let brk = if !main_node.is_null() {
            ffi::elf_get_brk_start(main_node)
        } else {
            highest
        };
        (*p).brk_start   = brk;
        (*p).brk_current = brk;
    } else {
        (*p).brk_start   = highest;
        (*p).brk_current = highest;
    }

    let ustack_top = (*p).ustack_virt + USER_STACK_BYTES;
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
        sp -= 4; ustack_write_u32(&*p, sp, 0); // auxv terminator (val)
        sp -= 4; ustack_write_u32(&*p, sp, 0); // auxv terminator (tag)
        for i in (0..6).rev() {
            sp -= 4; ustack_write_u32(&*p, sp, auxv[i].1);
            sp -= 4; ustack_write_u32(&*p, sp, auxv[i].0);
        }
    }

    push_empty_args(&*p, &mut sp);
    *stk.add(8) = sp;

    task_setup_sigreturn(t);

    irq_spinlock_acquire(&raw mut SCHEDULER_LOCK);
    task_list_add(t);
    mlfq::mlfq_enqueue_locked(t, (*t).priority);
    irq_spinlock_release(&raw mut SCHEDULER_LOCK);

    t
}
