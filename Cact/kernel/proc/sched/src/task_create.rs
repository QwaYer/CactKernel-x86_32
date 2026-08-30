//! Task creation: kernel tasks, plain user tasks, and ELF-backed processes
//! (static and dynamically linked).

use core::ffi::c_void;
use core::ptr;
use crate::ffi::{self, DynCtx, ProcPageTracker};
use crate::mlfq;
use crate::sync::{irq_spinlock_acquire, irq_spinlock_release};
use crate::task::{
    calc_highest_mapped_va, current_task, free_user_stack_pages, map_user_stack_in_pd,
    next_pid, push_empty_args, task_list_add, task_setup_sigreturn, task_zero_init,
    ProcMeta, TaskStruct, SCHEDULER_LOCK, KERNEL_BASE, KERNEL_STACK_SIZE,
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
pub unsafe extern "C" fn create_task_dynamic(
    entry:   *const c_void,
    pd:      *mut u32,
    tracker: *mut ProcPageTracker,
    ctx:     *mut DynCtx,
) -> *mut TaskStruct {
    let t = create_task_with_entry(entry, pd, tracker);
    if t.is_null() { return ptr::null_mut(); }

    irq_spinlock_acquire(&raw mut SCHEDULER_LOCK);
    (*(*t).proc).dyn_ctx = ctx;
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

    let mut new_dyn_ctx: *mut DynCtx = ptr::null_mut();
    let is_dynamic = ffi::elf_is_dynamic(path) != 0;
    let entry = if is_dynamic {
        new_dyn_ctx = ffi::dynlink_ctx_create(pd, &raw mut (*p).mm);
        if new_dyn_ctx.is_null() {
            ffi::printk(b"[INIT] dynlink_ctx_create failed\n\0".as_ptr());
            ffi::free_page((*p).stack_base);
            free_user_stack_pages(&mut *p);
            ffi::kfree(p as *mut c_void);
            ffi::kfree(t as *mut c_void);
            ffi::vmm_free_address_space(pd);
            return ptr::null_mut();
        }
        ffi::load_elf_dynamic(path, pd, &raw mut (*p).mm, new_dyn_ctx)
    } else {
        ffi::load_elf(path, pd, &raw mut (*p).mm)
    };
    if entry.is_null() {
        if !new_dyn_ctx.is_null() {
            ffi::dynlink_ctx_destroy(new_dyn_ctx);
        }
        ffi::free_page((*p).stack_base);
        free_user_stack_pages(&mut *p);
        ffi::kfree(p as *mut c_void);
        ffi::kfree(t as *mut c_void);
        ffi::vmm_free_address_space(pd);
        return ptr::null_mut();
    }

    (*t).page_directory = pd;
    (*p).dyn_ctx        = new_dyn_ctx;

    // Load symbol table for crash traces
    ffi::elf_load_exec_symtab(path, p as *mut c_void);

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
