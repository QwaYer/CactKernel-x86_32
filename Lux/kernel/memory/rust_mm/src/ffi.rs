#![allow(dead_code)]

pub const PAGE_SIZE: u32 = 4096;
pub const MEM_START: u32 = 0x100000;
pub const MEM_SIZE: u32 = 128 * 1024 * 1024;
pub const TOTAL_PAGES: u32 = MEM_SIZE / PAGE_SIZE;
pub const BITMAP_SIZE: u32 = TOTAL_PAGES / 8;
pub const HEAP_START: u32 = MEM_START + BITMAP_SIZE * PAGE_SIZE;
pub const HEAP_SIZE: u32 = 16 * 1024 * 1024;
pub const HEAP_MAGIC: u32 = 0xDEADBEEF;

pub const PAGE_PRESENT: u32 = 0x1;
pub const PAGE_RW: u32 = 0x2;
pub const PAGE_USER: u32 = 0x4;
pub const PAGE_COW: u32 = 0x200;
pub const PAGE_DEMAND: u32 = 0x400;
pub const PAGE_ZERO: u32 = 0x800;
pub const PAGE_SWAPPED: u32 = 0x200;
pub const PTE_ACCESSED: u32 = 0x20;

pub const USER_STACK_TOP: u32 = 0xC0000000;
pub const USER_STACK_LIMIT: u32 = 0xBF000000;
pub const USER_HEAP_START: u32 = 0x40000000;
pub const USER_HEAP_LIMIT: u32 = 0x80000000;

pub const PROT_NONE: i32 = 0x0;
pub const PROT_READ: i32 = 0x1;
pub const PROT_WRITE: i32 = 0x2;
pub const PROT_EXEC: i32 = 0x4;
pub const MAP_SHARED: i32 = 0x01;
pub const MAP_PRIVATE: i32 = 0x02;
pub const MAP_FIXED: i32 = 0x10;
pub const MAP_ANON: i32 = 0x20;
pub const MAP_FAILED: u32 = 0xFFFFFFFF;
pub const MMAP_BASE: u32 = 0x40000000;
pub const MMAP_LIMIT: u32 = 0xBF000000;
pub const MMAP_MAX_REGIONS: usize = 256;

pub const SHM_VA_BASE: u32 = 0xA0000000;
pub const SHM_VA_LIMIT: u32 = 0xB0000000;
pub const SHM_MAX_PAGES: usize = 64;
pub const SHM_MAX_SEGMENTS: usize = 32;
pub const TASK_SHM_MAX: usize = 16;
pub const IPC_CREAT: i32 = 0x0200;
pub const IPC_EXCL: i32 = 0x0400;
pub const IPC_PRIVATE: i32 = 0;
pub const IPC_RMID: i32 = 0;
pub const IPC_STAT: i32 = 2;
pub const SHM_RDONLY: i32 = 0x1000;
pub const SHM_RND: i32 = 0x2000;

pub const MAX_FD: usize = 256;
pub const NSIG: usize = 13;
pub const SIGKILL: u32 = 1 << 0;
pub const SIGSEGV: u32 = 1 << 8;

pub const SLAB_MAX_OBJ_SIZE: u32 = 2048;
pub const SLAB_MIN_OBJ_SIZE: u32 = 8;
pub const SLAB_NAME_LEN: usize = 32;

pub const SWAP_MAX_SLOTS: u32 = 65536;
pub const SWAP_BITMAP_SIZE: u32 = SWAP_MAX_SLOTS / 8;
pub const SWAP_DATA_START_LBA: u32 = 1;

pub const LOG_OK: u32 = 0;
pub const LOG_WARN: u32 = 1;
pub const LOG_ERROR: u32 = 2;
pub const LOG_FAIL: u32 = 3;

pub const COLOR_LIGHT_RED: u32 = 0xFF5555;

pub const TASK_READY: u32 = 0;
pub const TASK_RUNNING: u32 = 1;
pub const TASK_SLEEPING: u32 = 2;
pub const TASK_ZOMBIE: u32 = 3;
pub const TASK_WAITING: u32 = 4;

pub const PROC_INITIAL_PAGES: u32 = 256;
pub const PROC_GROW_STEP: u32 = 256;

#[inline(always)]
pub fn pd_index(vaddr: u32) -> u32 {
    (vaddr >> 22) & 0x3FF
}

#[inline(always)]
pub fn pt_index(vaddr: u32) -> u32 {
    (vaddr >> 12) & 0x3FF
}

#[repr(C)]
pub struct IrqSpinlock {
    pub spin_locked: u32,
    pub saved_flags: u32,
}

#[repr(C)]
pub struct VfsNode {
    _opaque: [u8; 0],
}

#[repr(C)]
pub struct ContextFrame {
    pub es: u32,
    pub ds: u32,
    pub edi: u32,
    pub esi: u32,
    pub ebp: u32,
    pub esp_dummy: u32,
    pub ebx: u32,
    pub edx: u32,
    pub ecx: u32,
    pub eax: u32,
    pub int_no: u32,
    pub err_code: u32,
    pub eip: u32,
    pub cs: u32,
    pub eflags: u32,
    pub useresp: u32,
    pub ss: u32,
}

#[repr(C)]
pub struct ShmAttachment {
    pub shm_id: i32,
    pub shm_vaddr: u32,
}

#[repr(C)]
pub struct ShmInfo {
    pub shm_segsz: u32,
    pub shm_cpid: u32,
    pub shm_lpid: u32,
    pub shm_nattch: u32,
}

#[repr(C)]
pub struct ProcPageTracker {
    pub pages: *mut *mut u8,
    pub count: u32,
    pub capacity: u32,
    pub page_dir: *mut u32,
}

#[repr(C)]
pub struct MmapRegion {
    pub base: u32,
    pub length: u32,
    pub flags: u32,
    pub prot: u32,
    pub fd: i32,
    pub file_off: u32,
    pub is_used: u8,
}

#[repr(C)]
pub struct MmapTable {
    pub regions: [MmapRegion; MMAP_MAX_REGIONS],
    pub next_base: u32,
}

#[repr(C)]
pub struct DynCtx {
    _opaque: [u8; 0],
}

#[repr(C)]
pub struct SchedQueue {
    pub head: *mut TaskStruct,
    pub tail: *mut TaskStruct,
    pub count: u32,
}

#[repr(C)]
pub struct TaskStruct {
    pub esp: u32,
    pub pid: u32,
    pub state: u32,
    pub is_kernel: u8,
    pub stack_base: *mut u8,
    pub ustack_phys: *mut u8,
    pub ustack_virt: u32,
    pub page_directory: *mut u32,
    pub next: *mut TaskStruct,
    pub queue_next: *mut TaskStruct,
    pub pending_signals: u32,
    pub signal_mask: u32,
    pub saved_signal_mask: u32,
    pub in_sigsuspend: u8,
    pub signal_handlers: [u32; NSIG],
    pub sigreturn_trampoline: u32,
    pub alarm_ticks: u32,
    pub itimer_value: u32,
    pub itimer_interval: u32,
    pub fd_table: [*mut VfsNode; MAX_FD],
    pub fd_offset: [u32; MAX_FD],
    pub fd_flags: [u32; MAX_FD],
    pub fd_cloexec: [u32; MAX_FD],
    pub mm: ProcPageTracker,
    pub mmap_table: MmapTable,
    pub dyn_ctx: *mut DynCtx,
    pub parent_pid: u32,
    pub exit_code: i32,
    pub wait_for_pid: u32,
    pub brk_start: u32,
    pub brk_current: u32,
    pub sleep_until: u32,
    pub cwd: [u8; 256],
    pub uid: u32,
    pub gid: u32,
    pub euid: u32,
    pub egid: u32,
    pub shm_attachments: [ShmAttachment; TASK_SHM_MAX],
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct PfStats {
    pub total_faults: u32,
    pub demand_allocs: u32,
    pub cow_copies: u32,
    pub stack_grows: u32,
    pub zero_pages: u32,
    pub swap_ins: u32,
    pub protection_faults: u32,
    pub invalid_access: u32,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct SwapStats {
    pub total_slots: u32,
    pub used_slots: u32,
    pub pages_swapped_out: u32,
    pub pages_swapped_in: u32,
    pub swap_failures: u32,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct OomStats {
    pub oom_kills: u32,
    pub pages_reclaimed: u32,
    pub last_killed_pid: u32,
}

extern "C" {
    pub fn kprint(msg: *const u8);
    pub fn kprint_color(msg: *const u8, color: u32);
    pub fn kprint_hex(n: u32);
    pub fn klog(level: u32, msg: *const u8);
    pub fn itoa(n: i32, buf: *mut u8);
    pub fn hex_to_ascii(n: u32, buf: *mut u8);

    pub fn irq_spinlock_init(lock: *mut IrqSpinlock);
    pub fn irq_spinlock_acquire(lock: *mut IrqSpinlock);
    pub fn irq_spinlock_release(lock: *mut IrqSpinlock);

    pub fn load_page_directory(dir: *mut u32);
    pub fn enable_paging();

    pub fn read_vfs(node: *mut VfsNode, off: u32, size: u32, buf: *mut u8) -> i32;

    pub static mut current_task: *mut TaskStruct;
    pub static mut task_list_head: *mut TaskStruct;
    pub static mut scheduler_lock: IrqSpinlock;
    pub static mut ready_queue: SchedQueue;

    pub fn task_signal(pid: u32, signal: u32);
    pub fn schedule();
    pub fn task_reap();
}

#[inline(always)]
pub unsafe fn read_cr2() -> u32 {
    let val: u32;
    core::arch::asm!("mov {}, cr2", out(reg) val, options(nomem, nostack));
    val
}

#[inline(always)]
pub unsafe fn get_current_pd() -> *mut u32 {
    let val: u32;
    core::arch::asm!("mov {}, cr3", out(reg) val, options(nomem, nostack));
    val as *mut u32
}

#[inline(always)]
pub unsafe fn tlb_flush(vaddr: u32) {
    core::arch::asm!("invlpg [{}]", in(reg) vaddr, options(nostack));
}

#[inline(always)]
pub unsafe fn tlb_flush_all() {
    core::arch::asm!(
        "mov eax, cr3",
        "mov cr3, eax",
        out("eax") _,
        options(nostack)
    );
}

pub unsafe fn sched_queue_push(q: *mut SchedQueue, t: *mut TaskStruct) {
    (*t).queue_next = core::ptr::null_mut();
    if (*q).tail.is_null() {
        (*q).head = t;
        (*q).tail = t;
    } else {
        (*(*q).tail).queue_next = t;
        (*q).tail = t;
    }
    (*q).count += 1;
}

pub unsafe fn sched_queue_remove(q: *mut SchedQueue, t: *mut TaskStruct) {
    let mut prev: *mut TaskStruct = core::ptr::null_mut();
    let mut cur = (*q).head;
    while !cur.is_null() {
        if cur == t {
            if !prev.is_null() {
                (*prev).queue_next = (*cur).queue_next;
            } else {
                (*q).head = (*cur).queue_next;
            }
            if (*q).tail == cur {
                (*q).tail = prev;
            }
            (*cur).queue_next = core::ptr::null_mut();
            (*q).count -= 1;
            return;
        }
        prev = cur;
        cur = (*cur).queue_next;
    }
}