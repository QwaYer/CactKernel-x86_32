use core::ptr;
use core::ffi::c_void;
use crate::ffi::{self, ContextFrame, ProcPageTracker, DynCtx, MmapTable, PAGE_PRESENT, PAGE_RW, PAGE_USER, PAGE_SIZE, LOG_OK, LOG_FAIL};
use crate::sync::irq_spinlock_t;
use crate::mlfq;
use crate::timer_wheel;

pub const MAX_FD:     usize = 256;
pub use cact_sync::task_abi::{NSIG, TASK_SHM_MAX};

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

/// User-mode stack: `USER_STACK_TOP` is the first byte *above* the mapping (4 KiB pages).
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

fn ustack_phys_page(t: &TaskStruct, idx: usize) -> *mut c_void {
    if idx == 0 {
        t.ustack_phys
    } else {
        t.ustack_phys_extra[idx - 1]
    }
}

fn ustack_kernel_byte_mut(t: &TaskStruct, uva: u32) -> *mut u8 {
    let base = t.ustack_virt;
    let off = uva.wrapping_sub(base) as usize;
    debug_assert!(off < USER_STACK_BYTES as usize);
    let pi = off / PAGE_SIZE as usize;
    let po = off % PAGE_SIZE as usize;
    unsafe { ustack_phys_page(t, pi).cast::<u8>().add(po) }
}

fn ustack_write_u32(t: &TaskStruct, uva: u32, val: u32) {
    unsafe {
        *(ustack_kernel_byte_mut(t, uva) as *mut u32) = val;
    }
}

fn map_user_stack_in_pd(pd: *mut u32, t: &TaskStruct) {
    if pd.is_null() {
        return;
    }
    unsafe {
        for i in 0..USER_STACK_PAGES {
            let vaddr = t.ustack_virt.wrapping_add(i.wrapping_mul(PAGE_SIZE));
            let phys = ustack_phys_page(t, i as usize) as u32;
            ffi::vmm_map(pd, vaddr, phys, PAGE_USER | PAGE_RW | PAGE_PRESENT);
        }
    }
}

fn free_user_stack_pages(t: &mut TaskStruct) {
    unsafe {
        for i in 0..USER_STACK_PAGES as usize {
            let p = ustack_phys_page(t, i);
            if !p.is_null() {
                ffi::kfree_page(p);
            }
        }
    }
    t.ustack_phys = ptr::null_mut();
    t.ustack_phys_extra = [ptr::null_mut(); 3];
}

fn task_zero_init(t: *mut TaskStruct) -> bool {
    if t.is_null() {
        return false;
    }
    unsafe {
        ffi::memory_set(t as *mut c_void, 0, core::mem::size_of::<TaskStruct>());
        let t = &mut *t;

        let fds = ffi::kmalloc(core::mem::size_of::<ffi::TaskFdTable>()) as *mut ffi::TaskFdTable;
        if fds.is_null() {
            return false;
        }
        ffi::memory_set(fds as *mut c_void, 0, core::mem::size_of::<ffi::TaskFdTable>());
        t.fds = fds;

        let mmap_tbl = ffi::kmalloc(core::mem::size_of::<MmapTable>()) as *mut MmapTable;
        if mmap_tbl.is_null() {
            ffi::kfree_heap(fds as *mut c_void);
            t.fds = ptr::null_mut();
            return false;
        }
        ffi::mmap_table_init(mmap_tbl);
        t.mmap_table = mmap_tbl;

        t.state      = TaskState::Ready;
        t.priority   = mlfq::MLFQ_LEVEL_INTERACTIVE;
        t.time_slice = mlfq::MLFQ_QUANTUM[mlfq::MLFQ_LEVEL_INTERACTIVE as usize];
        t.cwd[0]     = b'/';
        for i in 0..NSIG {
            t.signal_handlers[i] = SIG_DFL;
        }
        true
    }
}

#[no_mangle]
pub unsafe extern "C" fn task_init() {
    ffi::kprint(b"[SCHED] initializing MLFQ queues (4 levels)\n\0".as_ptr());
    current_task    = ptr::null_mut();
    task_list_head  = ptr::null_mut();
    task_list_tail  = ptr::null_mut();
    next_pid        = 1;

    crate::sync::irq_spinlock_init(&raw mut SCHEDULER_LOCK);
    mlfq::mlfq_init();
    timer_wheel::timer_wheel_global_init();

    ffi::klog(LOG_OK, b"scheduler queues ready\0".as_ptr());
}

#[no_mangle]
pub unsafe extern "C" fn init_scheduler() -> i32 {
    ffi::kprint(b"[SCHED] allocating idle task (pid=0)\n\0".as_ptr());

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
    (*idle).next          = idle;  
    (*idle).priority      = mlfq::MLFQ_LEVEL_BACKGROUND;
    (*idle).cwd[0]        = b'/';

    current_task    = idle;
    task_list_head  = idle;
    task_list_tail  = idle;

    ffi::klog(LOG_OK, b"scheduler ready: idle(0)\0".as_ptr());
    0
}

#[no_mangle]
pub unsafe extern "C" fn create_task(entry_point: *const c_void) -> *mut TaskStruct {
    let t = ffi::kmalloc(core::mem::size_of::<TaskStruct>()) as *mut TaskStruct;
    if t.is_null() { return ptr::null_mut(); }

    let stack = ffi::kalloc() as *mut u32;
    if stack.is_null() { ffi::kfree_heap(t as *mut c_void); return ptr::null_mut(); }

    if !task_zero_init(t) {
        ffi::kfree_page(stack as *mut c_void);
        ffi::kfree_heap(t as *mut c_void);
        return ptr::null_mut();
    }

    let stack_top = (stack as usize + KERNEL_STACK_SIZE) as *mut u32;

    let mut esp = stack_top;

    // kernel_task_trampoline will `ret` to this entry point
    esp = esp.sub(1); *esp = entry_point as u32;

    // return address for switch_to's `ret`
    esp = esp.sub(1); *esp = ffi::kernel_task_trampoline as *const () as u32;

    // switch_to callee-saved registers (pop ebx, pop esi, pop edi, pop ebp)
    esp = esp.sub(1); *esp = 0; // ebp
    esp = esp.sub(1); *esp = 0; // edi
    esp = esp.sub(1); *esp = 0; // esi
    esp = esp.sub(1); *esp = 0; // ebx

    (*t).esp           = esp as u32;
    (*t).stack_base    = stack as *mut c_void;
    (*t).pid           = next_pid;
    next_pid          += 1;
    (*t).is_kernel     = 1;
    (*t).page_directory = ptr::null_mut();

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

    let kstack = ffi::kalloc() as *mut u32;
    if kstack.is_null() { ffi::kfree_heap(t as *mut c_void); return ptr::null_mut(); }

    let mut ustack_pages: [*mut c_void; USER_STACK_PAGES as usize] =
        [ptr::null_mut(); USER_STACK_PAGES as usize];
    for i in 0..USER_STACK_PAGES as usize {
        let p = ffi::kalloc() as *mut c_void;
        if p.is_null() {
            for j in 0..i {
                ffi::kfree_page(ustack_pages[j]);
            }
            ffi::kfree_page(kstack as *mut c_void);
            ffi::kfree_heap(t as *mut c_void);
            return ptr::null_mut();
        }
        ustack_pages[i] = p;
    }

    if !task_zero_init(t) {
        for p in ustack_pages {
            ffi::kfree_page(p);
        }
        ffi::kfree_page(kstack as *mut c_void);
        ffi::kfree_heap(t as *mut c_void);
        return ptr::null_mut();
    }

    let ustack_virt: u32 = KERNEL_BASE - USER_STACK_BYTES;

    let stack_top = (kstack as usize + KERNEL_STACK_SIZE) as *mut u32;
    let mut esp = stack_top;

    // iretd frame for ring3 (user_task_trampoline will iretd using this)
    esp = esp.sub(1); *esp = USER_DATA_SEL;                        // ss
    esp = esp.sub(1); *esp = ustack_virt + USER_STACK_BYTES - 4;   // useresp
    esp = esp.sub(1); *esp = 0x0000_0202;                          // eflags (IF=1)
    esp = esp.sub(1); *esp = USER_CODE_SEL;                        // cs
    esp = esp.sub(1); *esp = entry_point as u32;                   // eip

    // return address for switch_to's `ret`
    esp = esp.sub(1); *esp = ffi::user_task_trampoline as *const () as u32;

    // switch_to callee-saved registers
    esp = esp.sub(1); *esp = 0; // ebp
    esp = esp.sub(1); *esp = 0; // edi
    esp = esp.sub(1); *esp = 0; // esi
    esp = esp.sub(1); *esp = 0; // ebx

    (*t).esp          = esp as u32;
    (*t).stack_base   = kstack as *mut c_void;
    (*t).ustack_phys  = ustack_pages[0];
    (*t).ustack_phys_extra =
        [ustack_pages[1], ustack_pages[2], ustack_pages[3]];
    (*t).ustack_virt  = ustack_virt;
    (*t).pid          = next_pid;
    next_pid         += 1;
    (*t).is_kernel    = 0;
    (*t).parent_pid   = if !current_task.is_null() { (*current_task).pid } else { 0 };
    (*t).uid  = if !current_task.is_null() { (*current_task).uid  } else { 0 };
    (*t).gid  = if !current_task.is_null() { (*current_task).gid  } else { 0 };
    (*t).euid = if !current_task.is_null() { (*current_task).euid } else { 0 };
    (*t).egid = if !current_task.is_null() { (*current_task).egid } else { 0 };

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

    ffi::memory_copy(&raw mut (*t).mm as *mut c_void, tracker as *const c_void,
                     core::mem::size_of::<ProcPageTracker>());
    (*t).page_directory = pd;

    // Исправить EIP на стеке (offset 5 от esp: ebx,esi,edi,ebp,trampoline,eip)
    let stk = (*t).esp as *mut u32;
    *stk.add(5) = entry as u32;

    // Маппинг user-стека в page directory
    map_user_stack_in_pd(pd, &*t);

    // Вычислить brk
    let highest = calc_highest_mapped_va(pd);
    (*t).brk_start   = highest;
    (*t).brk_current = highest;

    // Пустые args на user stack
    let ustack_top = (*t).ustack_virt + USER_STACK_BYTES;
    let mut sp = ustack_top - 4;
    push_empty_args(&*t, &mut sp);
    // Исправить useresp на стеке (offset 8)
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
    (*t).dyn_ctx = ctx;
    crate::sync::irq_spinlock_release(&raw mut SCHEDULER_LOCK);

    t
}

#[no_mangle]
pub unsafe extern "C" fn create_elf_task(path: *const u8) -> *mut TaskStruct {
    let pd = ffi::vmm_create_address_space();
    if pd.is_null() { return ptr::null_mut(); }

    let t = create_user_task_internal(ptr::null(), false);
    if t.is_null() { ffi::vmm_free_address_space(pd); return ptr::null_mut(); }

    ffi::proc_tracker_init(&raw mut (*t).mm);
    let entry = ffi::load_elf(path, pd, &raw mut (*t).mm);
    if entry.is_null() {
        ffi::kfree_page((*t).stack_base);
        free_user_stack_pages(&mut *t);
        ffi::kfree_heap(t as *mut c_void);
        ffi::vmm_free_address_space(pd);
        return ptr::null_mut();
    }

    (*t).page_directory = pd;

    let stk = (*t).esp as *mut u32;
    *stk.add(5) = entry as u32;

    map_user_stack_in_pd(pd, &*t);

    let highest = calc_highest_mapped_va(pd);
    (*t).brk_start   = highest;
    (*t).brk_current = highest;

    let ustack_top = (*t).ustack_virt + USER_STACK_BYTES;
    let mut sp = ustack_top - 4;
    push_empty_args(&*t, &mut sp);
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

    let kstack = ffi::kalloc() as *mut u32;
    if kstack.is_null() {
        ffi::kfree_heap(child as *mut c_void);
        ffi::vmm_free_address_space(child_pd);
        crate::sync::irq_spinlock_release(&raw mut SCHEDULER_LOCK);
        return ptr::null_mut();
    }

    let mut ustack_pages: [*mut c_void; USER_STACK_PAGES as usize] =
        [ptr::null_mut(); USER_STACK_PAGES as usize];
    for i in 0..USER_STACK_PAGES as usize {
        let p = ffi::kalloc() as *mut c_void;
        if p.is_null() {
            for j in 0..i {
                ffi::kfree_page(ustack_pages[j]);
            }
            ffi::kfree_page(kstack as *mut c_void);
            ffi::kfree_heap(child as *mut c_void);
            ffi::vmm_free_address_space(child_pd);
            crate::sync::irq_spinlock_release(&raw mut SCHEDULER_LOCK);
            return ptr::null_mut();
        }
        ustack_pages[i] = p;
    }

    // Скопировать всю структуру parent → child
    ffi::memory_copy(child as *mut c_void, parent as *const c_void,
                     core::mem::size_of::<TaskStruct>());

    (*child).pid            = next_pid;
    next_pid               += 1;
    (*child).state          = TaskState::Ready;
    (*child).stack_base     = kstack as *mut c_void;
    (*child).ustack_phys    = ustack_pages[0];
    (*child).ustack_phys_extra =
        [ustack_pages[1], ustack_pages[2], ustack_pages[3]];
    (*child).page_directory = child_pd;
    (*child).parent_pid     = (*parent).pid;
    (*child).exit_code      = 0;
    (*child).wait_for_pid   = 0;
    (*child).sleep_until    = 0;
    (*child).pending_signals = 0;
    (*child).queue_next     = ptr::null_mut();
    (*child).next           = ptr::null_mut();
    (*child).wait_next      = ptr::null_mut();
    (*child).dyn_ctx        = ptr::null_mut();
    (*child).ticks_used     = 0;

    ffi::proc_tracker_init(&raw mut (*child).mm);
    (*child).mm.page_dir = child_pd;

    // Allocate independent fds for child, copy parent's fd data
    let child_fds = ffi::kmalloc(core::mem::size_of::<ffi::TaskFdTable>()) as *mut ffi::TaskFdTable;
    if child_fds.is_null() {
        for p in ustack_pages {
            ffi::kfree_page(p);
        }
        ffi::kfree_page(kstack as *mut c_void);
        ffi::kfree_heap(child as *mut c_void);
        ffi::vmm_free_address_space(child_pd);
        crate::sync::irq_spinlock_release(&raw mut SCHEDULER_LOCK);
        return ptr::null_mut();
    }
    ffi::memory_copy(child_fds as *mut c_void, (*parent).fds as *const c_void,
                     core::mem::size_of::<ffi::TaskFdTable>());
    (*child).fds = child_fds;

    // Allocate independent mmap_table for child
    let child_mmap = ffi::kmalloc(core::mem::size_of::<MmapTable>()) as *mut MmapTable;
    if child_mmap.is_null() {
        ffi::kfree_heap(child_fds as *mut c_void);
        (*child).fds = ptr::null_mut();
        for p in ustack_pages {
            ffi::kfree_page(p);
        }
        ffi::kfree_page(kstack as *mut c_void);
        ffi::kfree_heap(child as *mut c_void);
        ffi::vmm_free_address_space(child_pd);
        crate::sync::irq_spinlock_release(&raw mut SCHEDULER_LOCK);
        return ptr::null_mut();
    }
    ffi::mmap_table_init(child_mmap);
    (*child).mmap_table = child_mmap;

    // Клонировать адресное пространство (COW)
    if !(*parent).page_directory.is_null() {
        ffi::vmm_fork_address_space((*parent).page_directory, child_pd);
    }

    // Клонировать mmap-таблицу (COW для private, share для shared)
    ffi::mmap_table_clone(
        (*parent).mmap_table,
        (*child).mmap_table,
        (*parent).page_directory,
        child_pd,
    );

    // Маппинг и копирование user stack (несколько физстраниц)
    for i in 0..USER_STACK_PAGES as usize {
        let vaddr = (*child).ustack_virt.wrapping_add((i as u32).wrapping_mul(PAGE_SIZE));
        let cphys = ustack_phys_page(&*child, i) as u32;
        ffi::vmm_map(child_pd, vaddr, cphys, PAGE_USER | PAGE_RW | PAGE_PRESENT);
        ffi::memory_copy(
            ustack_phys_page(&*child, i),
            ustack_phys_page(&*parent, i) as *const c_void,
            PAGE_SIZE as usize,
        );
    }

    // Инкрементировать refcount файловых дескрипторов
    for i in 0..MAX_FD {
        if !(*(*child).fds).fd_table[i].is_null() {
            ffi::open_vfs((*(*child).fds).fd_table[i]);
        }
    }

    // Сбросить SHM-вложения (дочерний процесс не наследует)
    for i in 0..TASK_SHM_MAX {
        (*child).shm_attachments[i].shm_id    = 0;
        (*child).shm_attachments[i].shm_vaddr = 0;
    }

    // Настроить kernel stack child для возврата из fork
    let stack_top_ptr = (kstack as usize + KERNEL_STACK_SIZE) as *mut u32;
    let mut esp_ptr = stack_top_ptr;

    // iretd frame (fork_task_trampoline will pop es/ds, popa, iretd)
    esp_ptr = esp_ptr.sub(1); *esp_ptr = (*regs).ss;
    esp_ptr = esp_ptr.sub(1); *esp_ptr = (*regs).useresp;
    esp_ptr = esp_ptr.sub(1); *esp_ptr = (*regs).eflags;
    esp_ptr = esp_ptr.sub(1); *esp_ptr = (*regs).cs;
    esp_ptr = esp_ptr.sub(1); *esp_ptr = (*regs).eip;
    esp_ptr = esp_ptr.sub(1); *esp_ptr = 0;               // eax = 0 (child)
    esp_ptr = esp_ptr.sub(1); *esp_ptr = (*regs).ecx;
    esp_ptr = esp_ptr.sub(1); *esp_ptr = (*regs).edx;
    esp_ptr = esp_ptr.sub(1); *esp_ptr = (*regs).ebx;
    esp_ptr = esp_ptr.sub(1); *esp_ptr = 0;               // esp_dummy
    esp_ptr = esp_ptr.sub(1); *esp_ptr = (*regs).ebp;
    esp_ptr = esp_ptr.sub(1); *esp_ptr = (*regs).esi;
    esp_ptr = esp_ptr.sub(1); *esp_ptr = (*regs).edi;
    esp_ptr = esp_ptr.sub(1); *esp_ptr = (*regs).ds;
    esp_ptr = esp_ptr.sub(1); *esp_ptr = (*regs).es;

    // switch_to frame: ret lands on fork_task_trampoline
    esp_ptr = esp_ptr.sub(1); *esp_ptr = ffi::fork_task_trampoline as *const () as u32;
    esp_ptr = esp_ptr.sub(1); *esp_ptr = 0; // ebp
    esp_ptr = esp_ptr.sub(1); *esp_ptr = 0; // edi
    esp_ptr = esp_ptr.sub(1); *esp_ptr = 0; // esi
    esp_ptr = esp_ptr.sub(1); *esp_ptr = 0; // ebx

    (*child).esp = esp_ptr as u32;

    // COW maps the parent's trampoline page read-only into child's PD.
    // Allocate a fresh writable page so child has its own trampoline
    // and proc_free_pages won't double-free the parent's physical page.
    task_setup_sigreturn(child);

    task_list_add(child);
    mlfq::mlfq_enqueue_locked(child, (*child).priority);

    {
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

// ── sched_task_exit ─────────────────────────────────────────────────────
// Called from C sys_exit.  Sets zombie state and yields immediately.
// schedule() sees Zombie, sends SIGCHLD, wakes the parent (if Waiting),
// and never re-enqueues this task.  sys_exit's hlt-loop is a safety net
// in case schedule() returns (e.g. no other runnable task yet).
#[no_mangle]
pub unsafe extern "C" fn sched_task_exit(exit_code: i32) {
    let t = current_task;
    if t.is_null() { return; }

    {
        let mut buf = [0u8; 12];
        ffi::kprint(b"[EXIT] pid=\0".as_ptr());
        ffi::itoa((*t).pid as i32, buf.as_mut_ptr());
        ffi::kprint(buf.as_ptr());
        ffi::kprint(b" code=\0".as_ptr());
        ffi::itoa(exit_code, buf.as_mut_ptr());
        ffi::kprint(buf.as_ptr());
        ffi::kprint(b" parent=\0".as_ptr());
        ffi::itoa((*t).parent_pid as i32, buf.as_mut_ptr());
        ffi::kprint(buf.as_ptr());
        ffi::kprint(b"\n\0".as_ptr());
    }

    // Hold the lock to atomically set zombie state and reparent children.
    // Disabling interrupts prevents the timer from calling schedule() between
    // these two steps, which would leave orphaned children with a dangling parent_pid.
    crate::sync::irq_spinlock_acquire(&raw mut SCHEDULER_LOCK);

    (*t).exit_code = exit_code;
    (*t).state = TaskState::Zombie;

    // Reparent all live and zombie children: set their parent_pid to 0 so
    // task_reap() can collect them without a parent ever calling waitpid().
    let my_pid = (*t).pid;
    let mut child = task_list_head;
    while !child.is_null() {
        let next_child = (*child).next;
        if (*child).parent_pid == my_pid {
            (*child).parent_pid = 0;
        }
        child = next_child;
    }

    crate::sync::irq_spinlock_release(&raw mut SCHEDULER_LOCK);

    crate::mlfq::schedule();
}

// ── sched_waitpid ───────────────────────────────────────────────────────
// Blocking: scan children for a zombie that matches target_pid.
//   • Found  → collect exit code, mark reaped, return child pid.
//   • Not found but live children exist → Waiting + schedule(); retry.
//   • No matching children at all → return -1.
// Same pattern as sched_sleep_ticks: set state, release lock, schedule().
#[no_mangle]
pub unsafe extern "C" fn sched_waitpid(target_pid: i32, status: *mut i32) -> i32 {
    let cur = current_task;
    if cur.is_null() { return -1; }

    {
        let mut buf = [0u8; 12];
        ffi::kprint(b"[WAIT] pid=\0".as_ptr());
        ffi::itoa((*cur).pid as i32, buf.as_mut_ptr());
        ffi::kprint(buf.as_ptr());
        ffi::kprint(b" target=\0".as_ptr());
        ffi::itoa(target_pid, buf.as_mut_ptr());
        ffi::kprint(buf.as_ptr());
        ffi::kprint(b"\n\0".as_ptr());
    }

    loop {
        crate::sync::irq_spinlock_acquire(&raw mut SCHEDULER_LOCK);

        let mut t = task_list_head;
        let mut found_child = false;

        while !t.is_null() {
            if (*t).parent_pid == (*cur).pid
                && (target_pid <= 0 || (*t).pid == target_pid as u32)
            {
                if matches!((*t).state, TaskState::Zombie) {
                    let child_pid  = (*t).pid;
                    let child_exit = (*t).exit_code;
                    // Remove from task list under the lock before releasing it,
                    // so no other code (task_reap, another waitpid) touches this node.
                    task_list_remove(t);
                    let to_free = t;
                    crate::sync::irq_spinlock_release(&raw mut SCHEDULER_LOCK);

                    {
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
            ffi::kprint(b"[WAIT] no children found!\n\0".as_ptr());
            crate::sync::irq_spinlock_release(&raw mut SCHEDULER_LOCK);
            return -1;
        }

        ffi::kprint(b"[WAIT] blocking, child alive\n\0".as_ptr());
        (*cur).state = TaskState::Waiting;
        crate::sync::irq_spinlock_release(&raw mut SCHEDULER_LOCK);
        crate::mlfq::schedule();
        // Woken by SIGCHLD — loop back and collect the zombie.
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

    if path.is_null() || path as u32 >= KERNEL_BASE { return -1; }

    let t = current_task;
    if t.is_null() || (*t).is_kernel != 0 { return -1; }

    crate::sync::irq_spinlock_acquire(&raw mut SCHEDULER_LOCK);

    // Выгрузить динамический линкер
    if !(*t).dyn_ctx.is_null() {
        let ctx = (*t).dyn_ctx;
        (*t).dyn_ctx = ptr::null_mut();
        ffi::dynlink_unload_all(ctx);
        ffi::kfree_heap(ctx as *mut c_void);
    }

    let new_pd = ffi::vmm_create_address_space();
    if new_pd.is_null() {
        crate::sync::irq_spinlock_release(&raw mut SCHEDULER_LOCK);
        return -1;
    }

    ffi::proc_tracker_init(&raw mut (*t).mm);
    crate::sync::irq_spinlock_release(&raw mut SCHEDULER_LOCK);

    let entry = ffi::load_elf(path, new_pd, &raw mut (*t).mm);
    if entry.is_null() {
        ffi::kprint(b"[EXEC] load_elf failed (null entry), aborting exec\n\0".as_ptr());
        ffi::vmm_free_address_space(new_pd);
        return -1;
    }

    {
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

    /* Пока CR3 ещё старый: copy_strings_to_ustack читает argv/envp по старым
     * пользовательским VA. Иначе после раннего switch_paging эти страницы в new_pd
     * отсутствуют — исполняется мусор / #GP при возврате в user с -1. */
    map_user_stack_in_pd(new_pd, &*t);

    for pi in 0..USER_STACK_PAGES as usize {
        let us = ustack_phys_page(&*t, pi) as *mut u8;
        ffi::memory_set(us as *mut c_void, 0, PAGE_SIZE as usize);
    }

    {
        let file = ffi::vfs_walk_path(ffi::vfs_root, path);
        if !file.is_null() {
            let brk = ffi::elf_get_brk_start(file);
            (*t).brk_start   = brk;
            (*t).brk_current = brk;
        }
    }

    map_sigreturn_trampoline_on_pd(t, new_pd);

    ffi::tss_entry.esp0 = (*t).stack_base as u32 + KERNEL_STACK_SIZE as u32;

    let ustack_top = (*t).ustack_virt + USER_STACK_BYTES;
    let mut sp     = ustack_top - 4;

    let mut argv_vaddrs: [u32; 256] = [0; 256];
    let mut envp_vaddrs: [u32; 256] = [0; 256];

    let argc = match copy_strings_to_ustack(argv, EXEC_MAX_ARGS, &*t, &mut sp, &mut argv_vaddrs) {
        Some(n) => n,
        None => {
            ffi::kprint(b"[EXEC] abort: argv copy / stack overflow\n\0".as_ptr());
            ffi::vmm_free_address_space(new_pd);
            crate::sync::irq_spinlock_release(&raw mut SCHEDULER_LOCK);
            return -1;
        }
    };
    let envc = match copy_strings_to_ustack(envp, EXEC_MAX_ENVS, &*t, &mut sp, &mut envp_vaddrs) {
        Some(n) => n,
        None => {
            ffi::kprint(b"[EXEC] abort: envp copy / stack overflow\n\0".as_ptr());
            ffi::vmm_free_address_space(new_pd);
            crate::sync::irq_spinlock_release(&raw mut SCHEDULER_LOCK);
            return -1;
        }
    };

    let ptr_overhead = (argc as u32 + envc as u32 + 5) * 4;
    if sp < (*t).ustack_virt + ptr_overhead {
        ffi::kprint(b"[EXEC] abort: stack layout preflight failed\n\0".as_ptr());
        ffi::vmm_free_address_space(new_pd);
        crate::sync::irq_spinlock_release(&raw mut SCHEDULER_LOCK);
        return -1;
    }

    sp -= 4;
    ustack_write_u32(&*t, sp, 0);
    for i in (0..envc).rev() {
        sp -= 4;
        ustack_write_u32(&*t, sp, envp_vaddrs[i]);
    }
    let envp_arr = sp;

    sp -= 4;
    ustack_write_u32(&*t, sp, 0);
    for i in (0..argc).rev() {
        sp -= 4;
        ustack_write_u32(&*t, sp, argv_vaddrs[i]);
    }
    let argv_arr = sp;

    sp -= 4; ustack_write_u32(&*t, sp, envp_arr);
    sp -= 4; ustack_write_u32(&*t, sp, argv_arr);
    sp -= 4; ustack_write_u32(&*t, sp, argc as u32);

    (*t).pending_signals = 0;
    for i in 0..NSIG { (*t).signal_handlers[i] = SIG_DFL; }

    ffi::shm_detach_all((*t).pid, new_pd);
    for i in 0..TASK_SHM_MAX {
        (*t).shm_attachments[i].shm_id    = 0;
        (*t).shm_attachments[i].shm_vaddr = 0;
    }

    ffi::mmap_table_init((*t).mmap_table);

    for i in 3..MAX_FD {
        if !(*(*t).fds).fd_table[i].is_null() && (*(*t).fds).fd_cloexec[i] != 0 {
            ffi::close_vfs((*(*t).fds).fd_table[i]);
            (*(*t).fds).fd_table[i]   = ptr::null_mut();
            (*(*t).fds).fd_offset[i]  = 0;
            (*(*t).fds).fd_flags[i]   = 0;
            (*(*t).fds).fd_cloexec[i] = 0;
        }
    }

    /* После shm/mmap и cloexec снова фиксируем стек с RW (USER|P без W даёт #PF err=0x07). */
    map_user_stack_in_pd(new_pd, &*t);

    let old_pd = (*t).page_directory;
    (*t).page_directory = new_pd;
    ffi::switch_paging(new_pd);
    if !old_pd.is_null() {
        ffi::vmm_free_address_space(old_pd);
    }

    /* Entire TLB was flushed by CR3 reload, but invalidate the image page
     * explicitly in case a hypervisor/CPU quirk leaves a stale mapping for
     * this linear address across AS switches. */
    unsafe {
        core::arch::asm!(
            "invlpg [{}]",
            in(reg) entry as u32,
            options(nostack),
        );
    }

    {
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

    ffi::kprint(b"[EXEC] committed: new AS; iretd next\n\0".as_ptr());

    crate::sync::irq_spinlock_release(&raw mut SCHEDULER_LOCK);

    /* CR3 = new_pd. Перед iretd снова загружаем тот же PD в asm. */
    ffi::terminal_fg_pid = (*t).pid;

    let pd_val = new_pd as u32;
    let entry_u = entry as u32;
    let sp_u = sp;

    {
        let mut hbuf = [0u8; 12];
        let ustack_hi = (*t).ustack_virt.wrapping_add(USER_STACK_BYTES);
        ffi::kprint(b"[EXEC] iretd esp=0x\0".as_ptr());
        ffi::hex_to_ascii(sp_u, hbuf.as_mut_ptr());
        ffi::kprint(hbuf.as_ptr());
        ffi::kprint(b" ustack_page=[0x\0".as_ptr());
        ffi::hex_to_ascii((*t).ustack_virt, hbuf.as_mut_ptr());
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
        if sp_u < (*t).ustack_virt || sp_u >= ustack_hi {
            ffi::kprint(b"[EXEC] WARNING: esp outside mapped ustack region\n\0".as_ptr());
        }
    }

    unsafe {
        // (NT/IOPL и др.) — иначе iretd может вести себя как nested-task return (#GP).
        // entry_u и sp_u — только в ecx/edx: иначе LLVM кладёт entry в eax, а mov eax,0x23
        // затирает его и на стек вместо EIP попадает 0x23 (селектор данных → #PF с eip=0x23).
        core::arch::asm!(
            "mov cr3, ebx",
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
            in("ebx") pd_val,
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

    (*t).pending_signals |= signal;

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

    if (*t).in_sigsuspend != 0
        && matches!((*t).state, TaskState::Sleeping)
        && (signal & !(*t).signal_mask) != 0
    {
        mlfq::mlfq_remove_from_sleep(t);
        (*t).state = TaskState::Ready;
        mlfq::mlfq_enqueue_locked(t, (*t).priority);
    }
}

#[no_mangle]
pub unsafe extern "C" fn task_handle_signals(t: *mut TaskStruct) {
    if t.is_null() || (*t).pending_signals == 0 { return; }

    if (*t).in_sigsuspend != 0 {
        (*t).signal_mask   = (*t).saved_signal_mask;
        (*t).in_sigsuspend = 0;
    }

    if (*t).pending_signals & SIGKILL != 0 {
        (*t).pending_signals = 0;
        task_signal_locked((*t).parent_pid, SIGCHLD);
        (*t).state = TaskState::Zombie;
        return;
    }

    if (*t).pending_signals & SIGSTOP != 0 {
        (*t).pending_signals &= !SIGSTOP;
        (*t).state = TaskState::Sleeping;
        return;
    }

    let deliverable = (*t).pending_signals & !(*t).signal_mask;
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
        if matches!((*t).state, TaskState::Zombie) {
            return;
        }

        (*t).pending_signals &= !sig;
        let handler = (*t).signal_handlers[handler_idx];

        if sig == SIGCONT {
            if matches!((*t).state, TaskState::Sleeping) {
                (*t).state = TaskState::Ready;
            }
            return;
        }

        if sig == SIGCHLD || sig == SIGWINCH {
            if handler != SIG_DFL && handler != SIG_IGN {
                (*t).pending_signals |= sig;
            }
            return;
        }

        if term_by_default && (handler == SIG_DFL || handler == SIG_IGN) {
            task_signal_locked((*t).parent_pid, SIGCHLD);
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
    if t.is_null() || signum >= NSIG as u32 || signum == 0 { return -1; }

    if handler != SIG_DFL && handler != SIG_IGN && handler >= KERNEL_BASE {
        return -1;
    }

    if (1u32 << signum) & SIG_UNCATCHABLE != 0 { return -1; }

    (*t).signal_handlers[signum as usize] = handler;
    0
}

#[no_mangle]
pub unsafe extern "C" fn task_sigprocmask(
    t:      *mut TaskStruct,
    how:    i32,
    set:    *const u32,
    oldset: *mut u32,
) -> i32 {
    if t.is_null() { return -1; }
    if !oldset.is_null() { *oldset = (*t).signal_mask; }
    if set.is_null() { return 0; }

    let new_mask = *set & !SIG_UNCATCHABLE;
    match how {
        0 => (*t).signal_mask |=  new_mask,  // SIG_BLOCK
        1 => (*t).signal_mask &= !new_mask,  // SIG_UNBLOCK
        2 => (*t).signal_mask  =  new_mask,  // SIG_SETMASK
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

            if (*cur).alarm_ticks != 0 && now >= (*cur).alarm_ticks {
                (*cur).alarm_ticks = 0;
                (*cur).pending_signals |= SIGALRM;
                wake_if_sigsuspend(cur, SIGALRM);
            }

            if (*cur).itimer_value != 0 && now >= (*cur).itimer_value {
                (*cur).pending_signals |= SIGALRM;
                (*cur).itimer_value = if (*cur).itimer_interval != 0 {
                    now + (*cur).itimer_interval
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
        if (*t).in_sigsuspend != 0
            && matches!((*t).state, TaskState::Sleeping)
            && (signal & !(*t).signal_mask) != 0
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
        if !(*t).fds.is_null() {
            for j in 0..MAX_FD {
                if !(*(*t).fds).fd_table[j].is_null() {
                    ffi::close_vfs((*(*t).fds).fd_table[j]);
                }
            }
            ffi::kfree_heap((*t).fds as *mut c_void);
            (*t).fds = ptr::null_mut();
        }

        if !(*t).mmap_table.is_null() {
            ffi::kfree_heap((*t).mmap_table as *mut c_void);
            (*t).mmap_table = ptr::null_mut();
        }

        if !(*t).dyn_ctx.is_null() {
            ffi::dynlink_unload_all((*t).dyn_ctx);
            ffi::kfree_heap((*t).dyn_ctx as *mut c_void);
        }

        ffi::shm_detach_all((*t).pid, (*t).page_directory);
        ffi::proc_free_pages(&raw mut (*t).mm);

        (*t).page_directory = ptr::null_mut();
        (*t).ustack_phys    = ptr::null_mut();
        (*t).ustack_phys_extra = [ptr::null_mut(); 3];

        if !(*t).stack_base.is_null() {
            ffi::kfree_page((*t).stack_base);
        }

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
            // Only reap if exit code already collected (parent_pid==0 set by waitpid)
            // or parent no longer exists (orphan whose parent exited without waitpid).
            let reapable = (*cur).parent_pid == 0
                || find_task_by_pid((*cur).parent_pid).is_null();
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
    let ns = core::mem::transmute::<u32, TaskState>(new_state);
    (*t).state = ns;
    match ns {
        TaskState::Ready => mlfq::mlfq_enqueue_locked(t, (*t).priority),
        _ => {}
    }
}

#[no_mangle]
pub unsafe extern "C" fn task_setup_sigreturn(t: *mut TaskStruct) {
    if t.is_null() || (*t).page_directory.is_null() {
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

        ffi::vmm_map(pd, tramp_vaddr, phys as u32, PAGE_USER | PAGE_RW | PAGE_PRESENT);
        (*t).sigreturn_trampoline = tramp_vaddr;
    }
}

#[no_mangle]
pub unsafe extern "C" fn list_tasks() {
    crate::sync::irq_spinlock_acquire(&raw mut SCHEDULER_LOCK);

    ffi::kprint(b"\nPID  STATE     TYPE      PRIO\n\0".as_ptr());
    ffi::kprint(b"---  --------  --------  ----\n\0".as_ptr());

    let mut cur = task_list_head;
    while !cur.is_null() {
        let mut buf = [0u8; 16];
        ffi::itoa((*cur).pid as i32, buf.as_mut_ptr());
        ffi::kprint(buf.as_ptr());
        let digits = if (*cur).pid < 10 { 1 } else if (*cur).pid < 100 { 2 } else { 3 };
        for _ in digits..5 { ffi::kprint(b" \0".as_ptr()); }

        let state_str: &[u8] = match (*cur).state {
            TaskState::Running  => b"running   \0",
            TaskState::Ready    => b"ready     \0",
            TaskState::Zombie   => b"zombie    \0",
            TaskState::Waiting  => b"waiting   \0",
            TaskState::Sleeping => b"sleeping  \0",
        };
        ffi::kprint(state_str.as_ptr());
        ffi::kprint(if (*cur).is_kernel != 0 { b"kernel    \0".as_ptr() } else { b"user      \0".as_ptr() });

        ffi::itoa((*cur).priority as i32, buf.as_mut_ptr());
        ffi::kprint(buf.as_ptr());
        ffi::kprint(b"\n\0".as_ptr());

        cur = (*cur).next;
    }

    crate::sync::irq_spinlock_release(&raw mut SCHEDULER_LOCK);
}

fn calc_highest_mapped_va(pd: *mut u32) -> u32 {
    if pd.is_null() {
        return 0;
    }
    unsafe {
        let mut highest = 0u32;
        for i in 1..768usize {
            if *pd.add(i) & PAGE_PRESENT == 0 {
                continue;
            }
            let pt = (*pd.add(i) & !0xFFF) as *const u32;
            for j in (0..1024usize).rev() {
                if *pt.add(j) & PAGE_PRESENT != 0 {
                    let va = ((i as u32) << 22) | ((j as u32) << 12);
                    if va < 0xBF00_0000 && va + PAGE_SIZE > highest {
                        highest = va + PAGE_SIZE;
                    }
                    break;
                }
            }
        }
        highest
    }
}

fn push_empty_args(t: &TaskStruct, sp: &mut u32) {
    *sp -= 4;
    ustack_write_u32(t, *sp, 0); // envp = NULL
    let envp_vaddr = *sp;

    *sp -= 4;
    ustack_write_u32(t, *sp, 0); // argv = NULL
    let argv_vaddr = *sp;

    *sp -= 4;
    ustack_write_u32(t, *sp, envp_vaddr);
    *sp -= 4;
    ustack_write_u32(t, *sp, argv_vaddr);
    *sp -= 4;
    ustack_write_u32(t, *sp, 0); // argc = 0
}

fn copy_strings_to_ustack(
    arr:    *mut *mut u8,
    max:    usize,
    t:      &TaskStruct,
    sp:     &mut u32,
    vaddrs: &mut [u32; 256],
) -> Option<usize> {
    if arr.is_null() {
        return Some(0);
    }
    let ustack_virt = t.ustack_virt;
    let mut count = 0usize;

    unsafe {
        for i in 0..max {
            let s = *arr.add(i);
            if s.is_null() {
                break;
            }

            let mut len = 0usize;
            while len < EXEC_MAX_STRLEN && *s.add(len) != 0 {
                len += 1;
            }
            len += 1;

            let new_sp = (*sp).wrapping_sub(len as u32) & !3u32;
            if new_sp >= *sp || new_sp < ustack_virt {
                return None;
            }
            *sp = new_sp;
            let base = *sp;
            for j in 0..len {
                *ustack_kernel_byte_mut(t, base.wrapping_add(j as u32)) = *s.add(j);
            }
            vaddrs[count] = *sp;
            count += 1;
        }
    }
    Some(count)
}