#![allow(dead_code)]

//! Physical layout constants, PTE/PDE flags, `extern "C"` kernel entry points, and boot MMAP types.
//!
//! This is the single source of truth for sizes and symbols shared between `rust_mm` and C.

use core::cell::UnsafeCell;

/// A `Sync`-safe transparent wrapper for `UnsafeCell<T>`, used for `extern` statics
/// that are mutated by C code under spinlock protection.
///
/// # Safety
///
/// All access through [`SyncMut::get`] is `unsafe` — the caller must ensure
/// the kernel's spinlock protocol is observed.
#[repr(transparent)]
pub struct SyncMut<T>(UnsafeCell<T>);

// SAFETY: kernel guarantees access is serialised via IrqSpinlock.
unsafe impl<T: Send> Sync for SyncMut<T> {}

impl<T> SyncMut<T> {
    pub fn get(&self) -> *mut T {
        self.0.get()
    }
}

pub const PAGE_SIZE: u32 = 4096;

/// Physical address where the kernel is loaded (1 MB mark).
pub const MEM_START: u32 = 0x0010_0000;

/// ---------------------------------------------------------------------------
/// 4 GB address-space layout
/// ---------------------------------------------------------------------------
///
/// We manage the full 32-bit physical address space (4 GB), minus the
/// PCI/MMIO hole that occupies the top ~512 MB (0xE000_0000 – 0xFFFF_FFFF).
/// The actual upper bound is capped to the last 32-bit address supported by
/// the hardware, which is conveyed by the Multiboot2 MMAP.  For sizing
/// static data structures we use the worst-case maximum.
///
///   0x0000_0000 – 0x000F_FFFF :  1 MB  BIOS / IVT / ROM (reserved)
///   0x0010_0000 – 0x01FF_FFFF : 31 MB  kernel text + BSS + static page tables
///   0x0200_0000 – 0x02FF_FFFF : 16 MB  heap window
///   0x0200_0000 – 0xBFFF_FFFF : ~3 GB  general-purpose physical RAM
///   0xC000_0000 – 0xFFFF_FFFF : PCI/MMIO hole — never touched by PMM
///
/// TOTAL_PAGES covers every 4K frame from address 0 up to PCI_HOLE_START.
/// ---------------------------------------------------------------------------

/// Upper boundary of the region managed by PMM (= start of PCI/MMIO hole).
/// 0xC000_0000 = 3072 MB (Q35 with 4 GB).  Change this if your board has
/// a different PCI hole location.
pub const PCI_HOLE_START: u32 = 0xC000_0000;

/// Manageable physical memory: 0 … PCI_HOLE_START.
pub const MEM_SIZE: u32 = PCI_HOLE_START; /* 3072 MB */

/// Total number of 4K pages in the managed range.
pub const TOTAL_PAGES: u32 = MEM_SIZE / PAGE_SIZE; /* 786 432 pages */

/// Bitmap byte count (1 bit per page).
pub const BITMAP_SIZE: u32 = TOTAL_PAGES / 8; /* 98 304 bytes ≈ 96 KB */

/// Heap starts right after the hard-reserved 32 MB low-memory region.
pub const HEAP_START: u32 = 32 * 1024 * 1024; /* 0x0200_0000 */
pub const HEAP_SIZE: u32 = 16 * 1024 * 1024;  /* 16 MB heap window */
pub const HEAP_MAGIC: u32 = 0xDEAD_BEEF;

/// Hard reservation: every physical page below RESERVED_END is permanently
/// marked "used" in the PMM bitmap so it is never handed out via kalloc().
///
/// 0 … 32 MB covers:
///   BIOS / IVT / ROM        0x0000_0000 – 0x000F_FFFF
///   Kernel text/data/BSS    0x0010_0000 – (kernel_end)
///   Static page-tables BSS  up to 0x00BF_FFFF
pub const RESERVED_END: u32 = 32 * 1024 * 1024; /* 0x0200_0000 */

/// Maximum number of MMAP entries copied from the MB2 boot info.
pub const MB2_MMAP_MAX_ENTRIES: usize = 128;
pub const MB2_MMAP_TYPE_AVAILABLE: u32 = 1;

pub const PAGE_PRESENT: u32 = 0x001;
pub const PAGE_RW:      u32 = 0x002;
pub const PAGE_USER:    u32 = 0x004;
/// Bit 8 in a PDE — marks a page table as privately allocated for this
/// process (as opposed to a shared static kernel page table).  The CPU
/// ignores this bit in PDEs; we use it to decide which page tables to
/// free in vmm_free_address_space and to COW-copy in vmm_fork.
/// Bit 8 is chosen because it is CPU-ignored for non-leaf PDEs (PS=0),
/// while bit 9 (0x200) is reserved for PAGE_COW on PTEs.
pub const PDE_PRIVATE:  u32 = 0x100;
/// Bit 3 — Page Write-Through (PWT).  Set for MMIO/PCI-hole PTEs so that
/// writes are not held in write buffers.  Also reused as PAGE_SWAPPED marker
/// when PRESENT=0 (non-overlapping use: hw only checks PWT when PRESENT=1).
pub const PAGE_PWT:     u32 = 0x008;
/// Alias: software swap marker reuses bit 3 (safe — PRESENT=0 when swapped).
pub const PAGE_SWAPPED: u32 = 0x008;
/// Bit 4 — Page Cache Disable (PCD).  Set for MMIO/PCI-hole PTEs so that
/// reads/writes bypass L1/L2 cache and reach device registers directly.
pub const PAGE_PCD:     u32 = 0x010;
/// Bit 7 — Page Attribute Table (PAT).  Together with PCD/PWT selects a
/// memory type from the IA32_PAT MSR.  (PAT=1,PCD=0,PWT=0) → entry 4 = WC.
/// Reserved for future Rust-side PAT usage; currently used by C pat.c.
//pub const PAGE_PAT:     u32 = 0x080;
pub const PTE_ACCESSED: u32 = 0x020;
pub const PAGE_COW:     u32 = 0x200;
pub const PAGE_DEMAND:  u32 = 0x400;
pub const PAGE_ZERO:    u32 = 0x800;

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
pub struct TaskFdTable {
    pub fd_table:   [*mut VfsNode; MAX_FD],
    pub fd_offset:  [u32; MAX_FD],
    pub fd_flags:   [u32; MAX_FD],
    pub fd_cloexec: [u32; MAX_FD],
}

#[repr(C)]
pub struct DynCtx {
    _opaque: [u8; 0],
}

#[repr(C)]
pub struct TaskStruct {
    pub esp:            u32,
    pub page_directory: *mut u32,
    pub fpu_context_ptr: *mut u8,
    pub pid:            u32,
    pub state:          u32,
    pub is_kernel:      u8,
    pub _pad0:          [u8; 3],
    pub next:           *mut TaskStruct,
    pub queue_next:     *mut TaskStruct,
    pub priority:       u32,
    pub time_slice:     u32,
    pub ticks_used:     u32,
    pub proc:           *mut ProcMeta,
}

#[repr(C)]
pub struct ProcMeta {
    pub stack_base:         *mut u8,
    pub ustack_phys:        *mut u8,
    pub ustack_virt:        u32,
    pub ustack_phys_extra:  [*mut u8; 3],
    pub pending_signals:    u32,
    pub signal_mask:        u32,
    pub saved_signal_mask:  u32,
    pub in_sigsuspend:      u8,
    pub _pad1:              [u8; 3],
    pub signal_handlers:    [u32; NSIG],
    pub sigreturn_trampoline: u32,
    pub alarm_ticks:        u32,
    pub itimer_value:       u32,
    pub itimer_interval:    u32,
    pub fds:                *mut TaskFdTable,
    pub mm:                 ProcPageTracker,
    pub mmap_table:         *mut MmapTable,
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
    pub shm_attachments:    [ShmAttachment; TASK_SHM_MAX],
    pub wait_next:          *mut TaskStruct,
    pub pgid:               u32,
    pub sid:                u32,
    pub umask:              u32,
    pub root:               *mut VfsNode,
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

/// Flat MMAP entry passed from the C multiboot2 parser.
/// Must match `mb2_mmap_flat_t` in multiboot2.h exactly.
/// base and len are 64-bit to preserve the full MB2 values (PAE-awareness).
#[repr(C)]
#[derive(Clone, Copy)]
pub struct Mb2MmapFlat {
    pub base: u64,
    pub len:  u64,
    pub ty:   u32,
}

/// Table of flat MMAP entries.
/// Must match `mb2_mmap_table_t` in multiboot2.h exactly.
#[repr(C)]
pub struct Mb2MmapTable {
    pub entries: [Mb2MmapFlat; MB2_MMAP_MAX_ENTRIES],
    pub count:   u32,
}

// SAFETY: Each function and static in this block is backed by a C definition
// in the kernel.  All pointer parameters must be non-null and correctly
// aligned.  `current_task`, `task_list_head`, and `scheduler_lock` are
// mutated under the scheduler spinlock — callers must hold that lock or be
// in a single-threaded context (boot / IRQ-off) before accessing them.
unsafe extern "C" {
    pub fn printk(msg: *const u8);
    pub fn printk_color(msg: *const u8, color: u32);
    pub fn printk_hex(n: u32);
    pub fn itoa(n: i32, buf: *mut u8);
    pub fn hex_to_ascii(n: u32, buf: *mut u8);

    pub fn irq_spinlock_init(lock: *mut IrqSpinlock);
    pub fn irq_spinlock_acquire(lock: *mut IrqSpinlock);
    pub fn irq_spinlock_release(lock: *mut IrqSpinlock);

    pub fn load_page_directory(dir: *mut u32);
    pub fn enable_paging();

    pub fn read_vfs(node: *mut VfsNode, off: u32, size: u32, buf: *mut u8) -> i32;

    pub static current_task: SyncMut<*mut TaskStruct>;
    pub static task_list_head: SyncMut<*mut TaskStruct>;
    pub static scheduler_lock: SyncMut<IrqSpinlock>;

    pub fn task_signal(pid: u32, signal: u32);
    pub fn schedule();
    pub fn task_reap();

    pub fn dump_context_frame(regs: *const ContextFrame, fault_addr: u32, signal: u32);
}

// ---------------------------------------------------------------------------
// Inline-asm helpers — encapsulated in safe functions
// ---------------------------------------------------------------------------

/// Read CR2 (page-fault address).
#[inline(always)]
pub fn read_cr2() -> u32 {
    let val: u32;
    // SAFETY: reading CR2 is side-effect-free.
    unsafe { core::arch::asm!("mov {}, cr2", out(reg) val, options(nomem, nostack)) };
    val
}

/// Read CR3 (current page directory).
#[inline(always)]
pub fn get_current_pd() -> *mut u32 {
    let val: u32;
    // SAFETY: reading CR3 is side-effect-free.
    unsafe { core::arch::asm!("mov {}, cr3", out(reg) val, options(nomem, nostack)) };
    val as *mut u32
}

/// Invalidate a single TLB entry.
#[inline(always)]
pub fn tlb_flush(vaddr: u32) {
    // SAFETY: INVLPG is safe for any address.
    unsafe { core::arch::asm!("invlpg [{}]", in(reg) vaddr, options(nostack)) };
}

/// Flush the entire TLB by reloading CR3.
#[inline(always)]
pub fn tlb_flush_all() {
    // SAFETY: reloading CR3 with the current value only flushes the TLB.
    unsafe {
        core::arch::asm!(
            "mov eax, cr3",
            "mov cr3, eax",
            out("eax") _,
            options(nostack)
        );
    }
}