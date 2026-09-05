use core::ffi::c_void;

use crate::kernel_types::{MmapTable, ProcPageTracker, TaskFdTable, VfsNode};

pub const NSIG: usize = 13;
pub const TASK_SHM_MAX: usize = 16;

/// Runnable lifecycle state stored in `TaskStruct.state` (u32 on the wire).
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

/// Lightweight hardware/scheduler context (48 bytes), matches C `task_struct`.
/// Process metadata (signals, fds, cwd, etc.) lives in `ProcMeta` on the heap.
#[repr(C)]
pub struct TaskStruct {
    pub esp:            u32,              //  0
    pub page_directory: *mut u32,         //  4
    pub fpu_context_ptr: *mut c_void,     //  8
    pub pid:            u32,              // 12
    pub state:          TaskState,        // 16 (u32)
    pub is_kernel:      u8,               // 20
    pub _pad0:          [u8; 3],          // 21
    pub next:           *mut TaskStruct,  // 24
    pub queue_next:     *mut TaskStruct,  // 28
    pub priority:       u32,              // 32
    pub time_slice:     u32,              // 36
    pub ticks_used:     u32,              // 40
    pub proc:           *mut ProcMeta,    // 44
}

/// Heap-allocated process metadata, matches C `proc_metadata_t`.
#[repr(C)]
pub struct ProcMeta {
    pub stack_base:         *mut c_void,
    pub ustack_phys:        *mut c_void,
    pub ustack_virt:        u32,
    pub ustack_phys_extra:  [*mut c_void; 3],
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
    pub pgid:               u32,
    pub sid:                u32,
    pub umask:              u32,
    pub root:               *mut VfsNode,
    // Cached symbol table from the main executable (for crash traces)
    pub exec_base:          u32,
    pub exec_symtab:        *mut u8,
    pub exec_strtab:        *mut u8,
    pub exec_symtab_count:  i32,
}

unsafe impl Send for TaskStruct {}
unsafe impl Sync for TaskStruct {}
unsafe impl Send for ProcMeta {}
unsafe impl Sync for ProcMeta {}
