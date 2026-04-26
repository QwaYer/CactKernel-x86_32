use core::ptr;
use core::ffi::c_void;
use crate::ffi::{self, ContextFrame, ProcPageTracker, DynCtx, MmapTable, PAGE_PRESENT, PAGE_RW, PAGE_USER, PAGE_SIZE, LOG_OK, LOG_FAIL};
use crate::sync::irq_spinlock_t;
use crate::mlfq;
use crate::timer_wheel;

pub const MAX_FD:     usize = 256;
pub const NSIG:       usize = 13;
pub const TASK_SHM_MAX: usize = 16;

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


#[repr(u32)]
#[derive(Copy, Clone, PartialEq, Eq, Debug)]
pub enum TaskState {
    Ready    = 0,
    Running  = 1,
    Sleeping = 2,
    Zombie   = 3,
    Waiting  = 4,
}


#[repr(C)]
#[derive(Copy, Clone)]
pub struct TaskShmAttach {
    pub shm_id:    u32,
    pub shm_vaddr: u32,
}

pub use crate::ffi::VfsNode;


#[repr(C)]
pub struct TaskStruct {
    pub esp:            u32,
    // offset 4
    pub pid:            u32,
    // offset 8
    pub state:          TaskState,
    // offset 12
    pub is_kernel:      u8,
    pub _pad0:          [u8; 3],   // выравнивание до 16
    // offset 16 — используется в interrupt.asm как [eax+16]
    pub stack_base:     *mut c_void,
    // offset 20
    pub ustack_phys:    *mut c_void,
    // offset 24
    pub ustack_virt:    u32,
    // offset 28 — используется в interrupt.asm как [eax+28]
    pub page_directory: *mut u32,
    // offset 32
    pub next:           *mut TaskStruct,
    // offset 36
    pub queue_next:     *mut TaskStruct,

    // === MLFQ-поля (не трогаются из C/asm) ===
    // offset 40
    pub priority:       u32,    // уровень MLFQ 0-3
    // offset 44
    pub time_slice:     u32,    // квант в тиках (не используется напрямую)
    // offset 48
    pub ticks_used:     u32,    // тиков использовано за текущий квант

    pub pending_signals:    u32,
    pub signal_mask:        u32,
    pub saved_signal_mask:  u32,
    pub in_sigsuspend:      u8,
    pub _pad1:              [u8; 3],
    pub signal_handlers:    [u32; NSIG],
    pub sigreturn_trampoline: u32,

    pub alarm_ticks:        u32,   // абсолютный тик для SIGALRM (0=нет)
    pub itimer_value:       u32,   // следующий fire абс. тик
    pub itimer_interval:    u32,   // интервал перезарядки

    pub fd_table:           [*mut VfsNode; MAX_FD],
    pub fd_offset:          [u32; MAX_FD],
    pub fd_flags:           [u32; MAX_FD],
    pub fd_cloexec:         [u32; MAX_FD],

    pub mm:                 ProcPageTracker,
    pub mmap_table:         MmapTable,
    pub dyn_ctx:            *mut DynCtx,

    pub parent_pid:         u32,
    pub exit_code:          i32,
    pub wait_for_pid:       u32,

    pub brk_start:          u32,
    pub brk_current:        u32,

    pub sleep_until:        u32,

    pub cwd:                [u8; 256],

    pub uid:                u32,
    pub gid:                u32,
    pub euid:               u32,
    pub egid:               u32,

    pub shm_attachments:    [TaskShmAttach; TASK_SHM_MAX],

    pub wait_next:          *mut TaskStruct,
}

unsafe impl Send for TaskStruct {}
unsafe impl Sync for TaskStruct {}


#[no_mangle]
pub static mut current_task: *mut TaskStruct = ptr::null_mut();

#[no_mangle]
pub static mut task_list_head: *mut TaskStruct = ptr::null_mut();

#[no_mangle]
pub static mut next_pid: u32 = 1;

#[no_mangle]
pub static mut SCHEDULER_LOCK: irq_spinlock_t = irq_spinlock_t::new();

#[no_mangle]
pub static mut scheduler_lock: *mut irq_spinlock_t = &raw mut SCHEDULER_LOCK;

static mut task_list_tail: *mut TaskStruct = ptr::null_mut();

pub unsafe fn task_list_add(t: *mut TaskStruct) {
    (*t).next = ptr::null_mut();
    if task_list_tail.is_null() {
        task_list_head = t;
        task_list_tail = t;
    } else {
        (*task_list_tail).next = t;
        task_list_tail = t;
    }
}

pub unsafe fn task_list_remove(t: *mut TaskStruct) {
    if task_list_head.is_null() || t.is_null() { return; }

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

pub unsafe fn find_task_by_pid(pid: u32) -> *mut TaskStruct {
    let mut cur = task_list_head;
    while !cur.is_null() {
        if (*cur).pid == pid { return cur; }
        cur = (*cur).next;
    }
    ptr::null_mut()
}


unsafe fn task_zero_init(t: *mut TaskStruct) {
    ffi::memory_set(t as *mut c_void, 0, core::mem::size_of::<TaskStruct>());

    (*t).state      = TaskState::Ready;
    (*t).priority   = mlfq::MLFQ_LEVEL_INTERACTIVE;
    (*t).time_slice = mlfq::MLFQ_QUANTUM[mlfq::MLFQ_LEVEL_INTERACTIVE as usize];
    (*t).cwd[0]     = b'/';
    for i in 0..NSIG {
        (*t).signal_handlers[i] = SIG_DFL;
    }
    ffi::mmap_table_init(&raw mut (*t).mmap_table);
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

    ffi::kprint(b"[SCHED] spawning terminal_task (pid=1)\n\0".as_ptr());

    // terminal_task объявлена в C-коде (kernel.c)
    extern "C" { fn terminal_task(); }
    create_task(terminal_task as *const c_void);

    ffi::klog(LOG_OK, b"scheduler ready: idle(0) + terminal(1)\0".as_ptr());
    0
}

#[no_mangle]
pub unsafe extern "C" fn create_task(entry_point: *const c_void) -> *mut TaskStruct {
    let t = ffi::kmalloc(core::mem::size_of::<TaskStruct>()) as *mut TaskStruct;
    if t.is_null() { return ptr::null_mut(); }

    let stack = ffi::kalloc() as *mut u32;
    if stack.is_null() { ffi::kfree_heap(t as *mut c_void); return ptr::null_mut(); }

    task_zero_init(t);

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

unsafe fn create_user_task_internal(entry_point: *const c_void, add_to_list: bool) -> *mut TaskStruct {
    let t = ffi::kmalloc(core::mem::size_of::<TaskStruct>()) as *mut TaskStruct;
    if t.is_null() { return ptr::null_mut(); }

    let kstack = ffi::kalloc() as *mut u32;
    if kstack.is_null() { ffi::kfree_heap(t as *mut c_void); return ptr::null_mut(); }

    let ustack_phys = ffi::kalloc() as *mut u32;
    if ustack_phys.is_null() {
        ffi::kfree_page(kstack as *mut c_void);
        ffi::kfree_heap(t as *mut c_void);
        return ptr::null_mut();
    }

    task_zero_init(t);

    let ustack_virt: u32 = 0xBFFF_F000;

    let stack_top = (kstack as usize + KERNEL_STACK_SIZE) as *mut u32;
    let mut esp = stack_top;

    // iretd frame for ring3 (user_task_trampoline will iretd using this)
    esp = esp.sub(1); *esp = USER_DATA_SEL;                        // ss
    esp = esp.sub(1); *esp = ustack_virt + PAGE_SIZE - 4;         // useresp
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
    (*t).ustack_phys  = ustack_phys as *mut c_void;
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
    ffi::vmm_map(pd, (*t).ustack_virt, (*t).ustack_phys as u32,
                 PAGE_USER | PAGE_RW | PAGE_PRESENT);

    // Вычислить brk
    let highest = calc_highest_mapped_va(pd);
    (*t).brk_start   = highest;
    (*t).brk_current = highest;

    // Пустые args на user stack
    let ustack_top = (*t).ustack_virt + PAGE_SIZE;
    let phys = (*t).ustack_phys as *mut u8;
    let mut sp = ustack_top - 4;
    push_empty_args(phys, (*t).ustack_virt, &mut sp);
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
        ffi::kfree_page((*t).ustack_phys);
        ffi::kfree_heap(t as *mut c_void);
        ffi::vmm_free_address_space(pd);
        return ptr::null_mut();
    }

    (*t).page_directory = pd;

    let stk = (*t).esp as *mut u32;
    *stk.add(5) = entry as u32;

    ffi::vmm_map(pd, (*t).ustack_virt, (*t).ustack_phys as u32,
                 PAGE_USER | PAGE_RW | PAGE_PRESENT);

    let highest = calc_highest_mapped_va(pd);
    (*t).brk_start   = highest;
    (*t).brk_current = highest;

    let ustack_top = (*t).ustack_virt + PAGE_SIZE;
    let phys = (*t).ustack_phys as *mut u8;
    let mut sp = ustack_top - 4;
    push_empty_args(phys, (*t).ustack_virt, &mut sp);
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

    let ustack_phys = ffi::kalloc() as *mut u32;
    if ustack_phys.is_null() {
        ffi::kfree_page(kstack as *mut c_void);
        ffi::kfree_heap(child as *mut c_void);
        ffi::vmm_free_address_space(child_pd);
        crate::sync::irq_spinlock_release(&raw mut SCHEDULER_LOCK);
        return ptr::null_mut();
    }

    // Скопировать всю структуру parent → child
    ffi::memory_copy(child as *mut c_void, parent as *const c_void,
                     core::mem::size_of::<TaskStruct>());

    (*child).pid            = next_pid;
    next_pid               += 1;
    (*child).state          = TaskState::Ready;
    (*child).stack_base     = kstack as *mut c_void;
    (*child).ustack_phys    = ustack_phys as *mut c_void;
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

    // КРИТИЧНО: после memcpy у ребёнка mm.pages и mm.page_dir указывают
    // на объекты родителя. Без сброса task_reap на зомби-ребёнке вызвал бы
    // proc_free_pages(&child.mm), что:
    //   1) освобождает родительский массив pages (heap corruption),
    //   2) вызывает vmm_free_address_space на PD РОДИТЕЛЯ.
    // Также mmap_table содержит копию параметров родителя; mmap_table_clone
    // ниже заполнит её заново от начального состояния.
    ffi::proc_tracker_init(&raw mut (*child).mm);
    (*child).mm.page_dir = child_pd;
    ffi::mmap_table_init(&raw mut (*child).mmap_table);

    // Клонировать адресное пространство (COW)
    if !(*parent).page_directory.is_null() {
        ffi::vmm_fork_address_space((*parent).page_directory, child_pd);
    }

    // Клонировать mmap-таблицу (COW для private, share для shared)
    ffi::mmap_table_clone(
        &raw mut (*parent).mmap_table,
        &raw mut (*child).mmap_table,
        (*parent).page_directory,
        child_pd,
    );

    // Маппинг и копирование user stack
    ffi::vmm_map(child_pd, (*child).ustack_virt, ustack_phys as u32,
                 PAGE_USER | PAGE_RW | PAGE_PRESENT);
    ffi::memory_copy(ustack_phys as *mut c_void, (*parent).ustack_phys as *const c_void, PAGE_SIZE as usize);

    // Инкрементировать refcount файловых дескрипторов
    for i in 0..MAX_FD {
        if !(*child).fd_table[i].is_null() {
            ffi::open_vfs((*child).fd_table[i]);
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

    (*t).exit_code = exit_code;
    (*t).state = TaskState::Zombie;

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
                    let child_pid = (*t).pid;
                    let child_exit = (*t).exit_code;
                    (*t).parent_pid = 0; // mark reaped — won't match again
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
        ffi::vmm_free_address_space(new_pd);
        return -1;
    }

    crate::sync::irq_spinlock_acquire(&raw mut SCHEDULER_LOCK);

    if !(*t).page_directory.is_null() {
        ffi::proc_free_pages(&raw mut (*t).mm);
        ffi::vmm_free_address_space((*t).page_directory);
    }

    (*t).page_directory  = new_pd;
    (*t).pending_signals = 0;
    for i in 0..NSIG { (*t).signal_handlers[i] = SIG_DFL; }

    ffi::shm_detach_all((*t).pid, (*t).page_directory);
    for i in 0..TASK_SHM_MAX {
        (*t).shm_attachments[i].shm_id    = 0;
        (*t).shm_attachments[i].shm_vaddr = 0;
    }

    ffi::mmap_table_init(&raw mut (*t).mmap_table);

    for i in 3..MAX_FD {
        if !(*t).fd_table[i].is_null() && (*t).fd_cloexec[i] != 0 {
            ffi::close_vfs((*t).fd_table[i]);
            (*t).fd_table[i]   = ptr::null_mut();
            (*t).fd_offset[i]  = 0;
            (*t).fd_flags[i]   = 0;
            (*t).fd_cloexec[i] = 0;
        }
    }

    // Маппинг user stack
    ffi::vmm_map(new_pd, (*t).ustack_virt, (*t).ustack_phys as u32,
                 PAGE_USER | PAGE_RW | PAGE_PRESENT);

    let us = (*t).ustack_phys as *mut u8;
    for i in 0..(PAGE_SIZE as usize) { *us.add(i) = 0; }

    {
        let file = ffi::vfs_walk_path(ffi::vfs_root, path);
        if !file.is_null() {
            let brk = ffi::elf_get_brk_start(file);
            (*t).brk_start   = brk;
            (*t).brk_current = brk;
        }
    }

    task_setup_sigreturn(t);

    // Обновить TSS.esp0
    ffi::tss_entry.esp0 = (*t).stack_base as u32 + KERNEL_STACK_SIZE as u32;

    // Построить стек с аргументами
    let ustack_top = (*t).ustack_virt + PAGE_SIZE;
    let phys_base  = (*t).ustack_phys as *mut u8;
    let mut sp     = ustack_top - 4;

    let mut argv_vaddrs: [u32; 256] = [0; 256];
    let mut envp_vaddrs: [u32; 256] = [0; 256];

    let argc = copy_strings_to_ustack(argv, EXEC_MAX_ARGS, phys_base, (*t).ustack_virt, &mut sp, &mut argv_vaddrs);
    let envc = copy_strings_to_ustack(envp, EXEC_MAX_ENVS, phys_base, (*t).ustack_virt, &mut sp, &mut envp_vaddrs);

    // envp[] array
    sp -= 4;
    *(phys_base.add((sp - (*t).ustack_virt) as usize) as *mut u32) = 0;
    for i in (0..envc).rev() {
        sp -= 4;
        *(phys_base.add((sp - (*t).ustack_virt) as usize) as *mut u32) = envp_vaddrs[i];
    }
    let envp_arr = sp;

    // argv[] array
    sp -= 4;
    *(phys_base.add((sp - (*t).ustack_virt) as usize) as *mut u32) = 0;
    for i in (0..argc).rev() {
        sp -= 4;
        *(phys_base.add((sp - (*t).ustack_virt) as usize) as *mut u32) = argv_vaddrs[i];
    }
    let argv_arr = sp;

    // main(argc, argv, envp)
    sp -= 4; *(phys_base.add((sp - (*t).ustack_virt) as usize) as *mut u32) = envp_arr;
    sp -= 4; *(phys_base.add((sp - (*t).ustack_virt) as usize) as *mut u32) = argv_arr;
    sp -= 4; *(phys_base.add((sp - (*t).ustack_virt) as usize) as *mut u32) = argc as u32;

    crate::sync::irq_spinlock_release(&raw mut SCHEDULER_LOCK);

    // Переключить страничную адресацию и прыгнуть в user-space
    ffi::switch_paging(new_pd);
    ffi::terminal_fg_pid = (*t).pid;

    core::arch::asm!(
        "mov eax, 0x23",
        "mov ds, ax",
        "mov es, ax",
        "push 0x23",    // ss
        "push {sp}",    // useresp
        "pushfd",
        "or dword ptr [esp], 0x200",  // IF=1
        "push 0x1B",    // cs
        "push {entry}", // eip
        "iretd",
        sp    = in(reg) sp,
        entry = in(reg) entry as u32,
        options(noreturn)
    );
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

unsafe fn handle_signal_bit(
    t:          *mut TaskStruct,
    deliverable: u32,
    sig:        u32,
    handler_idx: usize,
    term_by_default: bool,
) {
    if deliverable & sig == 0 { return; }
    if matches!((*t).state, TaskState::Zombie) { return; }

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

#[no_mangle]
pub unsafe extern "C" fn task_sigaction(
    t:       *mut TaskStruct,
    signum:  u32,
    handler: u32,
) -> i32 {
    if t.is_null() || signum >= NSIG as u32 || signum == 0 { return -1; }
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

unsafe fn check_alarm_timers() {
    let now = timer_wheel::timer_current_tick();
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
            } else { 0 };
            wake_if_sigsuspend(cur, SIGALRM);
        }

        cur = next;
    }
}

unsafe fn wake_if_sigsuspend(t: *mut TaskStruct, signal: u32) {
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
pub unsafe extern "C" fn task_reap() {
    let mut to_reap: [*mut TaskStruct; 64] = [ptr::null_mut(); 64];
    let mut count = 0usize;

    crate::sync::irq_spinlock_acquire(&raw mut SCHEDULER_LOCK);
    let mut cur = task_list_head;
    while !cur.is_null() && count < 64 {
        let next = (*cur).next;
        if matches!((*cur).state, TaskState::Zombie) {
            task_list_remove(cur);
            to_reap[count] = cur;
            count += 1;
        }
        cur = next;
    }
    crate::sync::irq_spinlock_release(&raw mut SCHEDULER_LOCK);
    for i in 0..count {
        let t = to_reap[i];

        for j in 0..MAX_FD {
            if !(*t).fd_table[j].is_null() {
                ffi::close_vfs((*t).fd_table[j]);
                (*t).fd_table[j] = ptr::null_mut();
            }
        }

        if !(*t).dyn_ctx.is_null() {
            ffi::dynlink_unload_all((*t).dyn_ctx);
            ffi::kfree_heap((*t).dyn_ctx as *mut c_void);
        }

        ffi::shm_detach_all((*t).pid, (*t).page_directory);

        ffi::proc_free_pages(&raw mut (*t).mm);

        (*t).page_directory = ptr::null_mut();
        (*t).ustack_phys    = ptr::null_mut();

        if !(*t).stack_base.is_null() { ffi::kfree_page((*t).stack_base); }

        ffi::kfree_heap(t as *mut c_void);
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
    if t.is_null() || (*t).page_directory.is_null() { return; }

    let tramp_vaddr: u32 = 0xBEFFF000;
    let phys = ffi::kalloc() as *mut u8;
    if phys.is_null() { return; }

    for i in 0..(PAGE_SIZE as usize) { *phys.add(i) = 0; }

    // Трамплин сигнала:
    // sub esp, 4          — восстановить esp до основания signal_frame_t
    *phys.add(0) = 0x83; *phys.add(1) = 0xEC; *phys.add(2) = 0x04;
    // mov eax, 16 (SYS_SIGRETURN)
    *phys.add(3) = 0xB8; *phys.add(4) = 0x10; *phys.add(5) = 0x00; *phys.add(6) = 0x00; *phys.add(7) = 0x00;
    // int 0x80
    *phys.add(8) = 0xCD; *phys.add(9) = 0x80;
    // hlt
    *phys.add(10) = 0xF4;

    ffi::vmm_map((*t).page_directory, tramp_vaddr, phys as u32,
                 PAGE_USER | PAGE_PRESENT);
    (*t).sigreturn_trampoline = tramp_vaddr;
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

unsafe fn calc_highest_mapped_va(pd: *mut u32) -> u32 {
    let mut highest = 0u32;
    for i in 1..768usize {
        if *pd.add(i) & PAGE_PRESENT == 0 { continue; }
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

unsafe fn push_empty_args(phys: *mut u8, ustack_virt: u32, sp: &mut u32) {
    *sp -= 4;
    *(phys.add((*sp - ustack_virt) as usize) as *mut u32) = 0;  // envp = NULL
    let envp_vaddr = *sp;

    *sp -= 4;
    *(phys.add((*sp - ustack_virt) as usize) as *mut u32) = 0;  // argv = NULL
    let argv_vaddr = *sp;

    *sp -= 4;
    *(phys.add((*sp - ustack_virt) as usize) as *mut u32) = envp_vaddr;
    *sp -= 4;
    *(phys.add((*sp - ustack_virt) as usize) as *mut u32) = argv_vaddr;
    *sp -= 4;
    *(phys.add((*sp - ustack_virt) as usize) as *mut u32) = 0;  // argc = 0
}

unsafe fn copy_strings_to_ustack(
    arr:         *mut *mut u8,
    max:         usize,
    phys_base:   *mut u8,
    ustack_virt: u32,
    sp:          &mut u32,
    vaddrs:      &mut [u32; 256],
) -> usize {
    if arr.is_null() { return 0; }
    let mut count = 0usize;

    for i in 0..max {
        let s = *arr.add(i);
        if s.is_null() { break; }

        let mut len = 0usize;
        while len < EXEC_MAX_STRLEN && *s.add(len) != 0 { len += 1; }
        len += 1;

        *sp -= len as u32;
        *sp &= !3u32; 
        let off = (*sp - ustack_virt) as usize;
        for j in 0..len {
            *phys_base.add(off + j) = *s.add(j);
        }
        vaddrs[count] = *sp;
        count += 1;
    }
    count
}