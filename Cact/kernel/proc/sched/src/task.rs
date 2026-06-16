use core::ptr;
use core::ffi::c_void;
use crate::ffi::{self, ContextFrame, ProcPageTracker, DynCtx, MmapTable, PAGE_PRESENT, PAGE_RW, PAGE_USER, PAGE_SIZE, LOG_FAIL, LOG_OK};
use crate::sync::irq_spinlock_t;
use crate::mlfq;
use crate::timer_wheel;

pub const MAX_FD:     usize = 256;
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
const TRACE_PROC_LOGS: bool = false;

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

fn ustack_phys_by_idx(p: &ProcMeta, idx: usize) -> *mut c_void {
    if idx == 0 {
        p.ustack_phys
    } else {
        p.ustack_phys_extra[idx - 1]
    }
}

fn ustack_kernel_byte_mut(p: &ProcMeta, uva: u32) -> *mut u8 {
    let base = p.ustack_virt;
    let off = uva.wrapping_sub(base) as usize;
    debug_assert!(off < USER_STACK_BYTES as usize);
    let pi = off / PAGE_SIZE as usize;
    let po = off % PAGE_SIZE as usize;
    unsafe { ustack_phys_by_idx(p, pi).cast::<u8>().add(po) }
}

fn ustack_write_u32(p: &ProcMeta, uva: u32, val: u32) {
    unsafe {
        *(ustack_kernel_byte_mut(p, uva) as *mut u32) = val;
    }
}

fn map_user_stack_in_pd(pd: *mut u32, p: &ProcMeta) {
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

fn free_user_stack_pages(p: &mut ProcMeta) {
    unsafe {
        for i in 0..USER_STACK_PAGES as usize {
            let pn = ustack_phys_by_idx(p, i);
            if !pn.is_null() {
                ffi::kfree_page(pn);
            }
        }
    }
    p.ustack_phys = ptr::null_mut();
    p.ustack_phys_extra = [ptr::null_mut(); 3];
}

fn task_zero_init(t: *mut TaskStruct, p: *mut ProcMeta) -> bool {
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
            ffi::kfree_heap(fds as *mut c_void);
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
    ffi::klog(
        LOG_OK,
        b"Task subsystem initialized (MLFQ, timer wheel, scheduler lock)\0".as_ptr(),
    );
}

#[no_mangle]
pub unsafe extern "C" fn init_scheduler() -> i32 {
    let idle = ffi::kmalloc(core::mem::size_of::<TaskStruct>()) as *mut TaskStruct;
    if idle.is_null() {
        ffi::klog(LOG_FAIL, b"cannot allocate idle task\0".as_ptr());
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

    ffi::klog(
        LOG_OK,
        b"Scheduler initialized (idle task pid 0, circular run queue)\0".as_ptr(),
    );
    0
}

#[no_mangle]
pub unsafe extern "C" fn create_task(entry_point: *const c_void) -> *mut TaskStruct {
    let t = ffi::kmalloc(core::mem::size_of::<TaskStruct>()) as *mut TaskStruct;
    if t.is_null() { return ptr::null_mut(); }

    let p = ffi::kmalloc(core::mem::size_of::<ProcMeta>()) as *mut ProcMeta;
    if p.is_null() { ffi::kfree_heap(t as *mut c_void); return ptr::null_mut(); }

    let stack = ffi::kalloc() as *mut u32;
    if stack.is_null() { ffi::kfree_heap(p as *mut c_void); ffi::kfree_heap(t as *mut c_void); return ptr::null_mut(); }

    if !task_zero_init(t, p) {
        ffi::kfree_page(stack as *mut c_void);
        ffi::kfree_heap(p as *mut c_void);
        ffi::kfree_heap(t as *mut c_void);
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

    crate::sync::irq_spinlock_acquire(&raw mut SCHEDULER_LOCK);
    task_list_add(t);
    mlfq::mlfq_enqueue_locked(t, (*t).priority);
    crate::sync::irq_spinlock_release(&raw mut SCHEDULER_LOCK);

    t
}

fn create_user_task_internal(entry_point: *const c_void, add_to_list: bool) -> *mut TaskStruct {
    unsafe {
    let t = ffi::kmalloc(core::mem::size_of::<TaskStruct>()) as *mut TaskStruct;
    if t.is_null() { return ptr::null_mut(); }

    let p = ffi::kmalloc(core::mem::size_of::<ProcMeta>()) as *mut ProcMeta;
    if p.is_null() { ffi::kfree_heap(t as *mut c_void); return ptr::null_mut(); }

    let kstack = ffi::kalloc() as *mut u32;
    if kstack.is_null() { ffi::kfree_heap(p as *mut c_void); ffi::kfree_heap(t as *mut c_void); return ptr::null_mut(); }

    let mut ustack_pages: [*mut c_void; USER_STACK_PAGES as usize] =
        [ptr::null_mut(); USER_STACK_PAGES as usize];
    for i in 0..USER_STACK_PAGES as usize {
        let page = ffi::kalloc() as *mut c_void;
        if page.is_null() {
            for j in 0..i {
                ffi::kfree_page(ustack_pages[j]);
            }
            ffi::kfree_page(kstack as *mut c_void);
            ffi::kfree_heap(p as *mut c_void);
            ffi::kfree_heap(t as *mut c_void);
            return ptr::null_mut();
        }
        ustack_pages[i] = page;
    }

    if !task_zero_init(t, p) {
        for page in ustack_pages {
            ffi::kfree_page(page);
        }
        ffi::kfree_page(kstack as *mut c_void);
        ffi::kfree_heap(p as *mut c_void);
        ffi::kfree_heap(t as *mut c_void);
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
        crate::sync::irq_spinlock_acquire(&raw mut SCHEDULER_LOCK);
        task_list_add(t);
        mlfq::mlfq_enqueue_locked(t, (*t).priority);
        crate::sync::irq_spinlock_release(&raw mut SCHEDULER_LOCK);
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

    crate::sync::irq_spinlock_acquire(&raw mut SCHEDULER_LOCK);
    task_list_add(t);
    mlfq::mlfq_enqueue_locked(t, (*t).priority);
    crate::sync::irq_spinlock_release(&raw mut SCHEDULER_LOCK);

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

    crate::sync::irq_spinlock_acquire(&raw mut SCHEDULER_LOCK);
    (*(*t).proc).dyn_ctx = ctx;
    crate::sync::irq_spinlock_release(&raw mut SCHEDULER_LOCK);

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
            ffi::kprint(b"[INIT] dynlink_ctx_create failed\n\0".as_ptr());
            ffi::kfree_page((*p).stack_base);
            free_user_stack_pages(&mut *p);
            ffi::kfree_heap(p as *mut c_void);
            ffi::kfree_heap(t as *mut c_void);
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
        ffi::kfree_page((*p).stack_base);
        free_user_stack_pages(&mut *p);
        ffi::kfree_heap(p as *mut c_void);
        ffi::kfree_heap(t as *mut c_void);
        ffi::vmm_free_address_space(pd);
        return ptr::null_mut();
    }

    (*t).page_directory = pd;
    (*p).dyn_ctx        = new_dyn_ctx;

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

    crate::sync::irq_spinlock_acquire(&raw mut SCHEDULER_LOCK);
    task_list_add(t);
    mlfq::mlfq_enqueue_locked(t, (*t).priority);
    crate::sync::irq_spinlock_release(&raw mut SCHEDULER_LOCK);

    t
}

#[no_mangle]
pub unsafe extern "C" fn task_fork(regs: *mut ContextFrame) -> *mut TaskStruct {
    crate::sync::irq_spinlock_acquire(&raw mut SCHEDULER_LOCK);

    let parent = current_task;
    if parent.is_null() {
        crate::sync::irq_spinlock_release(&raw mut SCHEDULER_LOCK);
        return ptr::null_mut();
    }

    let child_pd = ffi::vmm_create_address_space();
    if child_pd.is_null() {
        crate::sync::irq_spinlock_release(&raw mut SCHEDULER_LOCK);
        return ptr::null_mut();
    }

    let child = ffi::kmalloc(core::mem::size_of::<TaskStruct>()) as *mut TaskStruct;
    if child.is_null() {
        ffi::vmm_free_address_space(child_pd);
        crate::sync::irq_spinlock_release(&raw mut SCHEDULER_LOCK);
        return ptr::null_mut();
    }

    let child_p = ffi::kmalloc(core::mem::size_of::<ProcMeta>()) as *mut ProcMeta;
    if child_p.is_null() {
        ffi::kfree_heap(child as *mut c_void);
        ffi::vmm_free_address_space(child_pd);
        crate::sync::irq_spinlock_release(&raw mut SCHEDULER_LOCK);
        return ptr::null_mut();
    }

    let kstack = ffi::kalloc() as *mut u32;
    if kstack.is_null() {
        ffi::kfree_heap(child_p as *mut c_void);
        ffi::kfree_heap(child as *mut c_void);
        ffi::vmm_free_address_space(child_pd);
        crate::sync::irq_spinlock_release(&raw mut SCHEDULER_LOCK);
        return ptr::null_mut();
    }

    let mut ustack_pages: [*mut c_void; USER_STACK_PAGES as usize] =
        [ptr::null_mut(); USER_STACK_PAGES as usize];
    for i in 0..USER_STACK_PAGES as usize {
        let page = ffi::kalloc() as *mut c_void;
        if page.is_null() {
            for j in 0..i {
                ffi::kfree_page(ustack_pages[j]);
            }
            ffi::kfree_page(kstack as *mut c_void);
            ffi::kfree_heap(child_p as *mut c_void);
            ffi::kfree_heap(child as *mut c_void);
            ffi::vmm_free_address_space(child_pd);
            crate::sync::irq_spinlock_release(&raw mut SCHEDULER_LOCK);
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
    (*child_p).dyn_ctx        = ptr::null_mut();

    ffi::proc_tracker_init(&raw mut (*child_p).mm);
    (*child_p).mm.page_dir = child_pd;

    let child_fds = ffi::kmalloc(core::mem::size_of::<ffi::TaskFdTable>()) as *mut ffi::TaskFdTable;
    if child_fds.is_null() {
        for page in ustack_pages {
            ffi::kfree_page(page);
        }
        ffi::kfree_page(kstack as *mut c_void);
        ffi::kfree_heap(child_p as *mut c_void);
        ffi::kfree_heap(child as *mut c_void);
        ffi::vmm_free_address_space(child_pd);
        crate::sync::irq_spinlock_release(&raw mut SCHEDULER_LOCK);
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
        ffi::kfree_heap(child_fds as *mut c_void);
        (*child_p).fds = ptr::null_mut();
        for page in ustack_pages {
            ffi::kfree_page(page);
        }
        ffi::kfree_page(kstack as *mut c_void);
        ffi::kfree_heap(child_p as *mut c_void);
        ffi::kfree_heap(child as *mut c_void);
        ffi::vmm_free_address_space(child_pd);
        crate::sync::irq_spinlock_release(&raw mut SCHEDULER_LOCK);
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
        ffi::kprint(b"[FORK] parent=\0".as_ptr());
        ffi::itoa((*parent).pid as i32, buf.as_mut_ptr());
        ffi::kprint(buf.as_ptr());
        ffi::kprint(b" child=\0".as_ptr());
        ffi::itoa((*child).pid as i32, buf.as_mut_ptr());
        ffi::kprint(buf.as_ptr());
        ffi::kprint(b" eip=0x\0".as_ptr());
        ffi::hex_to_ascii((*regs).eip, buf.as_mut_ptr());
        ffi::kprint(buf.as_ptr());
        ffi::kprint(b" uesp=0x\0".as_ptr());
        ffi::hex_to_ascii((*regs).useresp, buf.as_mut_ptr());
        ffi::kprint(buf.as_ptr());
        ffi::kprint(b" cs=0x\0".as_ptr());
        ffi::hex_to_ascii((*regs).cs, buf.as_mut_ptr());
        ffi::kprint(buf.as_ptr());
        ffi::kprint(b" ss=0x\0".as_ptr());
        ffi::hex_to_ascii((*regs).ss, buf.as_mut_ptr());
        ffi::kprint(buf.as_ptr());
        ffi::kprint(b" child_esp=0x\0".as_ptr());
        ffi::hex_to_ascii((*child).esp, buf.as_mut_ptr());
        ffi::kprint(buf.as_ptr());
        ffi::kprint(b"\n\0".as_ptr());
    }

    crate::sync::irq_spinlock_release(&raw mut SCHEDULER_LOCK);

    child
}

#[no_mangle]
pub unsafe extern "C" fn sched_task_exit(exit_code: i32) {
    let t = current_task;
    if t.is_null() { return; }

    let p = (*t).proc;

    if TRACE_PROC_LOGS {
        let mut buf = [0u8; 12];
        ffi::kprint(b"[EXIT] pid=\0".as_ptr());
        ffi::itoa((*t).pid as i32, buf.as_mut_ptr());
        ffi::kprint(buf.as_ptr());
        ffi::kprint(b" code=\0".as_ptr());
        ffi::itoa(exit_code, buf.as_mut_ptr());
        ffi::kprint(buf.as_ptr());
        ffi::kprint(b" parent=\0".as_ptr());
        ffi::itoa((*p).parent_pid as i32, buf.as_mut_ptr());
        ffi::kprint(buf.as_ptr());
        ffi::kprint(b"\n\0".as_ptr());
    }

    crate::sync::irq_spinlock_acquire(&raw mut SCHEDULER_LOCK);

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

    crate::sync::irq_spinlock_release(&raw mut SCHEDULER_LOCK);

    crate::mlfq::schedule();
}

#[no_mangle]
pub unsafe extern "C" fn sched_waitpid(target_pid: i32, status: *mut i32) -> i32 {
    let cur = current_task;
    if cur.is_null() { return -1; }

    loop {
        crate::sync::irq_spinlock_acquire(&raw mut SCHEDULER_LOCK);

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
                    crate::sync::irq_spinlock_release(&raw mut SCHEDULER_LOCK);

                    if TRACE_PROC_LOGS {
                        let mut buf = [0u8; 12];
                        ffi::kprint(b"[WAIT] reaped pid=\0".as_ptr());
                        ffi::itoa(child_pid as i32, buf.as_mut_ptr());
                        ffi::kprint(buf.as_ptr());
                        ffi::kprint(b" exit=\0".as_ptr());
                        ffi::itoa(child_exit, buf.as_mut_ptr());
                        ffi::kprint(buf.as_ptr());
                        ffi::kprint(b"\n\0".as_ptr());
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
                ffi::kprint(b"[WAIT] no children found!\n\0".as_ptr());
            }
            crate::sync::irq_spinlock_release(&raw mut SCHEDULER_LOCK);
            return -1;
        }

        if TRACE_PROC_LOGS {
            ffi::kprint(b"[WAIT] blocking, child alive\n\0".as_ptr());
        }
        (*cur).state = TaskState::Waiting;
        crate::sync::irq_spinlock_release(&raw mut SCHEDULER_LOCK);
        crate::mlfq::schedule();
    }
}

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
                ffi::kfree_page(new_ustack_pages[j]);
            }
            return -1;
        }
        new_ustack_pages[i] = page;
    }

    crate::sync::irq_spinlock_acquire(&raw mut SCHEDULER_LOCK);

    if !(*p).dyn_ctx.is_null() {
        ffi::dynlink_ctx_destroy((*p).dyn_ctx);
        (*p).dyn_ctx = ptr::null_mut();
    }

    let new_pd = ffi::vmm_create_address_space();
    if new_pd.is_null() {
        for page in new_ustack_pages {
            ffi::kfree_page(page);
        }
        crate::sync::irq_spinlock_release(&raw mut SCHEDULER_LOCK);
        return -1;
    }

    ffi::proc_tracker_init(&raw mut (*p).mm);
    crate::sync::irq_spinlock_release(&raw mut SCHEDULER_LOCK);

    let mut new_dyn_ctx: *mut DynCtx = ptr::null_mut();
    let is_dynamic = ffi::elf_is_dynamic(path) != 0;

    // Check execute permission before loading
    {
        let exec_node = ffi::vfs_walk_path(unsafe { *ffi::vfs_root.get() }, path);
        if exec_node.is_null() || ffi::vfs_check_perm(exec_node, 0x01) < 0 {
            if !new_dyn_ctx.is_null() {
                ffi::dynlink_ctx_destroy(new_dyn_ctx);
            }
            ffi::vmm_free_address_space(new_pd);
            for page in new_ustack_pages {
                ffi::kfree_page(page);
            }
            return -1;
        }
    }

    let entry = if is_dynamic {
        new_dyn_ctx = ffi::dynlink_ctx_create(new_pd, &raw mut (*p).mm);
        if new_dyn_ctx.is_null() {
            ffi::kprint(b"[EXEC] dynlink_ctx_create failed\n\0".as_ptr());
            ffi::vmm_free_address_space(new_pd);
            for page in new_ustack_pages {
                ffi::kfree_page(page);
            }
            return -1;
        }
        ffi::load_elf_dynamic(path, new_pd, &raw mut (*p).mm, new_dyn_ctx)
    } else {
        ffi::load_elf(path, new_pd, &raw mut (*p).mm)
    };
    if entry.is_null() {
        if !new_dyn_ctx.is_null() {
            ffi::dynlink_ctx_destroy(new_dyn_ctx);
        }
        ffi::vmm_free_address_space(new_pd);
        for page in new_ustack_pages {
            ffi::kfree_page(page);
        }
        return -1;
    }

    if TRACE_PROC_LOGS {
        let mut hbuf = [0u8; 12];
        ffi::kprint(b"[EXEC] load_elf entry=0x\0".as_ptr());
        ffi::hex_to_ascii(entry as u32, hbuf.as_mut_ptr());
        ffi::kprint(hbuf.as_ptr());
        ffi::kprint(b" pid=\0".as_ptr());
        ffi::itoa((*t).pid as i32, hbuf.as_mut_ptr());
        ffi::kprint(hbuf.as_ptr());
        ffi::kprint(b"\n\0".as_ptr());

        let pdi = (entry as usize) >> 22;
        let pti = ((entry as usize) >> 12) & 0x3FF;
        let pde = unsafe { *new_pd.add(pdi) };
        ffi::kprint(b"[EXEC] PDE for entry pdi=\0".as_ptr());
        ffi::itoa(pdi as i32, hbuf.as_mut_ptr());
        ffi::kprint(hbuf.as_ptr());
        ffi::kprint(b": 0x\0".as_ptr());
        ffi::hex_to_ascii(pde, hbuf.as_mut_ptr());
        ffi::kprint(hbuf.as_ptr());
        if pde == 0 || pde & PAGE_PRESENT == 0 {
            ffi::kprint(b" ABSENT (no exec mapping)\n\0".as_ptr());
        } else {
            ffi::kprint(b" OK\n\0".as_ptr());
            let pt = (pde & !0xFFFu32) as *mut u32;
            let pte = unsafe { *pt.add(pti) };
            ffi::kprint(b"[EXEC] PTE for entry pdi=\0".as_ptr());
            ffi::itoa(pdi as i32, hbuf.as_mut_ptr());
            ffi::kprint(hbuf.as_ptr());
            ffi::kprint(b" pti=\0".as_ptr());
            ffi::itoa(pti as i32, hbuf.as_mut_ptr());
            ffi::kprint(hbuf.as_ptr());
            ffi::kprint(b": 0x\0".as_ptr());
            ffi::hex_to_ascii(pte, hbuf.as_mut_ptr());
            ffi::kprint(hbuf.as_ptr());
            if pte == 0 || pte & PAGE_PRESENT == 0 {
                ffi::kprint(b" ABSENT (page not mapped)\n\0".as_ptr());
            } else {
                ffi::kprint(b" OK\n\0".as_ptr());
            }
        }
    }

    crate::sync::irq_spinlock_acquire(&raw mut SCHEDULER_LOCK);

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

    unsafe { (*ffi::tss_entry.get()).esp0 = (*p).stack_base as u32 + KERNEL_STACK_SIZE as u32; }

    let ustack_top = (*p).ustack_virt + USER_STACK_BYTES;
    let mut sp     = ustack_top - 4;

    let mut argv_vaddrs: [u32; 256] = [0; 256];
    let mut envp_vaddrs: [u32; 256] = [0; 256];

    let argc = match copy_strings_to_ustack(argv, EXEC_MAX_ARGS, &*p, &mut sp, &mut argv_vaddrs) {
        Some(n) => n,
        None => {
            ffi::kprint(b"[EXEC] abort: argv copy / stack overflow\n\0".as_ptr());
            (*p).ustack_phys = old_ustack_pages[0];
            (*p).ustack_phys_extra = [old_ustack_pages[1], old_ustack_pages[2], old_ustack_pages[3]];
            if !new_dyn_ctx.is_null() {
                ffi::dynlink_ctx_destroy(new_dyn_ctx);
            }
            ffi::vmm_free_address_space(new_pd);
            crate::sync::irq_spinlock_release(&raw mut SCHEDULER_LOCK);
            return -1;
        }
    };
    let envc = match copy_strings_to_ustack(envp, EXEC_MAX_ENVS, &*p, &mut sp, &mut envp_vaddrs) {
        Some(n) => n,
        None => {
            ffi::kprint(b"[EXEC] abort: envp copy / stack overflow\n\0".as_ptr());
            (*p).ustack_phys = old_ustack_pages[0];
            (*p).ustack_phys_extra = [old_ustack_pages[1], old_ustack_pages[2], old_ustack_pages[3]];
            if !new_dyn_ctx.is_null() {
                ffi::dynlink_ctx_destroy(new_dyn_ctx);
            }
            ffi::vmm_free_address_space(new_pd);
            crate::sync::irq_spinlock_release(&raw mut SCHEDULER_LOCK);
            return -1;
        }
    };

    let ptr_overhead = (argc as u32 + envc as u32 + 5) * 4;
    if sp < (*p).ustack_virt + ptr_overhead {
        ffi::kprint(b"[EXEC] abort: stack layout preflight failed\n\0".as_ptr());
        (*p).ustack_phys = old_ustack_pages[0];
        (*p).ustack_phys_extra = [old_ustack_pages[1], old_ustack_pages[2], old_ustack_pages[3]];
        if !new_dyn_ctx.is_null() {
            ffi::dynlink_ctx_destroy(new_dyn_ctx);
        }
        ffi::vmm_free_address_space(new_pd);
        crate::sync::irq_spinlock_release(&raw mut SCHEDULER_LOCK);
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
    (*p).dyn_ctx = new_dyn_ctx;

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
            ffi::kprint(b"[EXEC] POST-FREE entry map not user: PDE=0x\0".as_ptr());
            ffi::hex_to_ascii(pde, hbuf.as_mut_ptr());
            ffi::kprint(hbuf.as_ptr());
            ffi::kprint(b" PTE=0x\0".as_ptr());
            ffi::hex_to_ascii(pte, hbuf.as_mut_ptr());
            ffi::kprint(hbuf.as_ptr());
            ffi::kprint(b"\n\0".as_ptr());
        }
    }

    if TRACE_PROC_LOGS {
        ffi::kprint(b"[EXEC] committed: new AS; iretd next\n\0".as_ptr());
    }

    crate::sync::irq_spinlock_release(&raw mut SCHEDULER_LOCK);

    unsafe { *ffi::terminal_fg_pid.get() = (*t).pid; }

    let pd_val = new_pd as u32;
    let entry_u = entry as u32;
    let sp_u = sp;

    if TRACE_PROC_LOGS {
        let mut hbuf = [0u8; 12];
        let ustack_hi = (*p).ustack_virt.wrapping_add(USER_STACK_BYTES);
        ffi::kprint(b"[EXEC] iretd esp=0x\0".as_ptr());
        ffi::hex_to_ascii(sp_u, hbuf.as_mut_ptr());
        ffi::kprint(hbuf.as_ptr());
        ffi::kprint(b" ustack_page=[0x\0".as_ptr());
        ffi::hex_to_ascii((*p).ustack_virt, hbuf.as_mut_ptr());
        ffi::kprint(hbuf.as_ptr());
        ffi::kprint(b"..0x\0".as_ptr());
        ffi::hex_to_ascii(ustack_hi, hbuf.as_mut_ptr());
        ffi::kprint(hbuf.as_ptr());
        ffi::kprint(b") e_entry=0x\0".as_ptr());
        ffi::hex_to_ascii(entry_u, hbuf.as_mut_ptr());
        ffi::kprint(hbuf.as_ptr());
        ffi::kprint(b"\n\0".as_ptr());

        const ADDR_HINT: u32 = 0x0800_0EAD;
        if sp_u.abs_diff(ADDR_HINT) < 0x1000 {
            ffi::kprint(
                b"[EXEC] WARNING: esp within 4KiB of 0x08000EAD (stack vs code?)\n\0".as_ptr(),
            );
        }
        if sp_u >= 0x0800_0000 && sp_u < 0x0B00_0000 {
            ffi::kprint(
                b"[EXEC] WARNING: esp in low 0x0800_0000..0x0B00_0000 (expect ~0xBFF...)\n\0".as_ptr(),
            );
        }
        if sp_u < (*p).ustack_virt || sp_u >= ustack_hi {
            ffi::kprint(b"[EXEC] WARNING: esp outside mapped ustack region\n\0".as_ptr());
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

#[no_mangle]
pub unsafe extern "C" fn task_kill(pid: u32) {
    if pid == 0 { return; }
    task_signal(pid, SIGKILL);
}

#[no_mangle]
pub unsafe extern "C" fn task_signal(pid: u32, signal: u32) {
    crate::sync::irq_spinlock_acquire(&raw mut SCHEDULER_LOCK);
    task_signal_locked(pid, signal);
    crate::sync::irq_spinlock_release(&raw mut SCHEDULER_LOCK);
}

#[no_mangle]
pub unsafe extern "C" fn task_signal_locked(pid: u32, signal: u32) {
    if task_list_head.is_null() || pid == 0 { return; }

    let t = find_task_by_pid(pid);
    if t.is_null() { return; }

    let p = (*t).proc;

    (*p).pending_signals |= signal;

    if signal & (SIGKILL | SIGSTOP) != 0 {
        match (*t).state {
            TaskState::Sleeping => {
                mlfq::mlfq_remove_from_sleep(t);
                (*t).state = TaskState::Ready;
                mlfq::mlfq_enqueue_locked(t, (*t).priority);
            }
            TaskState::Waiting => {
                (*t).state = TaskState::Ready;
                mlfq::mlfq_enqueue_locked(t, (*t).priority);
            }
            _ => {}
        }
        return;
    }

    if (*p).in_sigsuspend != 0
        && matches!((*t).state, TaskState::Sleeping)
        && (signal & !(*p).signal_mask) != 0
    {
        mlfq::mlfq_remove_from_sleep(t);
        (*t).state = TaskState::Ready;
        mlfq::mlfq_enqueue_locked(t, (*t).priority);
    }
}

#[no_mangle]
pub unsafe extern "C" fn task_handle_signals(t: *mut TaskStruct) {
    if t.is_null() || (*t).proc.is_null() { return; }

    let p = (*t).proc;

    if (*p).pending_signals == 0 { return; }

    if (*p).in_sigsuspend != 0 {
        (*p).signal_mask   = (*p).saved_signal_mask;
        (*p).in_sigsuspend = 0;
    }

    if (*p).pending_signals & SIGKILL != 0 {
        (*p).pending_signals = 0;
        task_signal_locked((*p).parent_pid, SIGCHLD);
        (*t).state = TaskState::Zombie;
        return;
    }

    if (*p).pending_signals & SIGSTOP != 0 {
        (*p).pending_signals &= !SIGSTOP;
        (*t).state = TaskState::Sleeping;
        return;
    }

    let deliverable = (*p).pending_signals & !(*p).signal_mask;
    if deliverable == 0 { return; }

    handle_signal_bit(t, deliverable, SIGTERM,  1,  true);
    handle_signal_bit(t, deliverable, SIGCONT,  3,  false);
    handle_signal_bit(t, deliverable, SIGALRM,  5,  true);
    handle_signal_bit(t, deliverable, SIGCHLD,  6,  false);
    handle_signal_bit(t, deliverable, SIGFPE,   7,  true);
    handle_signal_bit(t, deliverable, SIGSEGV,  8,  true);
    handle_signal_bit(t, deliverable, SIGWINCH, 9,  false);
    handle_signal_bit(t, deliverable, SIGHUP,   10, true);
    handle_signal_bit(t, deliverable, SIGINT,   11, true);
    handle_signal_bit(t, deliverable, SIGQUIT,  12, true);
}

fn handle_signal_bit(
    t:          *mut TaskStruct,
    deliverable: u32,
    sig:        u32,
    handler_idx: usize,
    term_by_default: bool,
) {
    if t.is_null() {
        return;
    }
    if deliverable & sig == 0 {
        return;
    }
    unsafe {
        let p = (*t).proc;
        if matches!((*t).state, TaskState::Zombie) {
            return;
        }

        (*p).pending_signals &= !sig;
        let handler = (*p).signal_handlers[handler_idx];

        if sig == SIGCONT {
            if matches!((*t).state, TaskState::Sleeping) {
                (*t).state = TaskState::Ready;
            }
            return;
        }

        if sig == SIGCHLD || sig == SIGWINCH {
            if handler != SIG_DFL && handler != SIG_IGN {
                (*p).pending_signals |= sig;
            }
            return;
        }

        if term_by_default && (handler == SIG_DFL || handler == SIG_IGN) {
            task_signal_locked((*p).parent_pid, SIGCHLD);
            (*t).state = TaskState::Zombie;
        }
    }
}

#[no_mangle]
pub unsafe extern "C" fn task_sigaction(
    t:       *mut TaskStruct,
    signum:  u32,
    handler: u32,
) -> i32 {
    if t.is_null() || (*t).proc.is_null() || signum >= NSIG as u32 || signum == 0 { return -1; }

    let p = (*t).proc;

    if handler != SIG_DFL && handler != SIG_IGN && handler >= KERNEL_BASE {
        return -1;
    }

    if (1u32 << signum) & SIG_UNCATCHABLE != 0 { return -1; }

    (*p).signal_handlers[signum as usize] = handler;
    0
}

#[no_mangle]
pub unsafe extern "C" fn task_sigprocmask(
    t:      *mut TaskStruct,
    how:    i32,
    set:    *const u32,
    oldset: *mut u32,
) -> i32 {
    if t.is_null() || (*t).proc.is_null() { return -1; }
    let p = (*t).proc;
    if !oldset.is_null() { *oldset = (*p).signal_mask; }
    if set.is_null() { return 0; }

    let new_mask = *set & !SIG_UNCATCHABLE;
    match how {
        0 => (*p).signal_mask |=  new_mask,
        1 => (*p).signal_mask &= !new_mask,
        2 => (*p).signal_mask  =  new_mask,
        _ => return -1,
    }
    0
}

#[no_mangle]
pub unsafe extern "C" fn task_check_timers() {
    timer_wheel::timer_wheel_tick();
    check_alarm_timers();
}

fn check_alarm_timers() {
    let now = timer_wheel::timer_current_tick();

    unsafe {
        crate::sync::irq_spinlock_acquire(&raw mut SCHEDULER_LOCK);

        let mut cur = task_list_head;
        while !cur.is_null() {
            let next = (*cur).next;
            let p = (*cur).proc;
            if p.is_null() { cur = next; continue; }

            if (*p).alarm_ticks != 0 && now >= (*p).alarm_ticks {
                (*p).alarm_ticks = 0;
                (*p).pending_signals |= SIGALRM;
                wake_if_sigsuspend(cur, SIGALRM);
            }

            if (*p).itimer_value != 0 && now >= (*p).itimer_value {
                (*p).pending_signals |= SIGALRM;
                (*p).itimer_value = if (*p).itimer_interval != 0 {
                    now + (*p).itimer_interval
                } else {
                    0
                };
                wake_if_sigsuspend(cur, SIGALRM);
            }

            cur = next;
        }

        crate::sync::irq_spinlock_release(&raw mut SCHEDULER_LOCK);
    }
}

fn wake_if_sigsuspend(t: *mut TaskStruct, signal: u32) {
    if t.is_null() {
        return;
    }
    unsafe {
        let p = (*t).proc;
        if (*p).in_sigsuspend != 0
            && matches!((*t).state, TaskState::Sleeping)
            && (signal & !(*p).signal_mask) != 0
        {
            mlfq::mlfq_remove_from_sleep(t);
            (*t).state = TaskState::Ready;
            mlfq::mlfq_enqueue_locked(t, (*t).priority);
        }
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
            ffi::kfree_heap((*p).fds as *mut c_void);
            (*p).fds = ptr::null_mut();
        }

        if !(*p).mmap_table.is_null() {
            let mt = (*p).mmap_table;
            let pd = (*t).page_directory;
            if !pd.is_null() {
                ffi::mmap_table_free(mt, pd);
            }
            ffi::kfree_heap(mt as *mut c_void);
            (*p).mmap_table = ptr::null_mut();
        }

        if !(*p).dyn_ctx.is_null() {
            ffi::dynlink_ctx_destroy((*p).dyn_ctx);
            (*p).dyn_ctx = ptr::null_mut();
        }

        ffi::shm_detach_all((*t).pid, (*t).page_directory);
        ffi::proc_free_pages(&raw mut (*p).mm);

        (*t).page_directory = ptr::null_mut();
        (*p).ustack_phys    = ptr::null_mut();
        (*p).ustack_phys_extra = [ptr::null_mut(); 3];

        if !(*p).stack_base.is_null() {
            ffi::kfree_page((*p).stack_base);
        }

        ffi::kfree_heap(p as *mut c_void);
        ffi::kfree_heap(t as *mut c_void);
    }
}

#[no_mangle]
pub unsafe extern "C" fn task_reap() {
    let mut to_reap: [*mut TaskStruct; 64] = [ptr::null_mut(); 64];
    let mut count = 0usize;

    crate::sync::irq_spinlock_acquire(&raw mut SCHEDULER_LOCK);
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
    crate::sync::irq_spinlock_release(&raw mut SCHEDULER_LOCK);

    for i in 0..count {
        reap_task_free(to_reap[i]);
    }
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

#[no_mangle]
pub unsafe extern "C" fn task_setup_sigreturn(t: *mut TaskStruct) {
    if t.is_null() || (*t).page_directory.is_null() || (*t).proc.is_null() {
        return;
    }
    map_sigreturn_trampoline_on_pd(t, (*t).page_directory);
}

fn map_sigreturn_trampoline_on_pd(t: *mut TaskStruct, pd: *mut u32) {
    if t.is_null() || pd.is_null() {
        return;
    }

    unsafe {
        let tramp_vaddr: u32 = 0xBEFFF000;
        let phys = ffi::kalloc() as *mut u8;
        if phys.is_null() {
            return;
        }

        for i in 0..(PAGE_SIZE as usize) {
            *phys.add(i) = 0;
        }
        *phys.add(0) = 0x83;
        *phys.add(1) = 0xEC;
        *phys.add(2) = 0x04;
        let sigret_num: u32 = ffi::sys_sigreturn_num;
        *phys.add(3) = 0xB8;
        *phys.add(4) = (sigret_num & 0xFF) as u8;
        *phys.add(5) = ((sigret_num >> 8) & 0xFF) as u8;
        *phys.add(6) = ((sigret_num >> 16) & 0xFF) as u8;
        *phys.add(7) = ((sigret_num >> 24) & 0xFF) as u8;
        *phys.add(8) = 0xCD;
        *phys.add(9) = 0x80;
        *phys.add(10) = 0xF4;

        let vmm_flags = if (*t).is_kernel != 0 {
            PAGE_PRESENT | PAGE_RW
        } else {
            PAGE_PRESENT | PAGE_RW | PAGE_USER
        };
        ffi::vmm_map(pd, tramp_vaddr, phys as u32, vmm_flags);

        if !(*t).proc.is_null() {
            (*(*t).proc).sigreturn_trampoline = tramp_vaddr;
        }
    }
}

fn calc_highest_mapped_va(pd: *mut u32) -> u32 {
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

fn push_empty_args(p: &ProcMeta, sp: &mut u32) {
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
