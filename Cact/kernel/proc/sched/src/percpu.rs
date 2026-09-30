//! Per-CPU scheduler state.
//!
//! The scheduler historically kept a single global `current_task`, `fpu_owner`
//! and re-entry guard, which is only meaningful while one core runs tasks.  This
//! module gives every logical CPU its own copy of that state so worker cores can
//! be brought into the scheduler (Step B of the energy plan).
//!
//! `smp_self_cpu()` derives the index from the loaded TR, so the BSP is CPU 0
//! and each AP reports the index the trampoline stamped into its TSS.  Before an
//! AP has loaded its per-CPU GDT (`ltr`) it would report a negative index; it is
//! clamped to 0, and no AP touches this state before `ltr` with IRQs on.
//!
//! The per-CPU *runqueues* are still the single global MLFQ: this module only
//! removes the "one running task kernel-wide" assumption.  Until the worker
//! cores run `schedule()` themselves, every CPU except the BSP keeps `current`
//! pointing at its own idle task.

use core::cell::SyncUnsafeCell;
use core::ptr;
use core::sync::atomic::{AtomicU32, Ordering};

use crate::task::TaskStruct;

/// Number of logical CPUs the per-CPU table covers.  Must match `smp::MAX_CPUS`.
pub const MAX_CPUS: usize = 64;

/// One CPU's private scheduler state.
#[repr(C)]
pub struct CpuSched {
    /// Task currently running on this CPU (null only before the scheduler starts).
    pub current: *mut TaskStruct,
    /// This CPU's idle task (the fallback context for the CPU).
    pub idle: *mut TaskStruct,
    /// Task owning the FPU/SSE state on this CPU (lazy-FPU bookkeeping).
    pub fpu_owner: *mut TaskStruct,
    /// Re-entry guard for `schedule()` on this CPU.
    pub schedule_in_progress: AtomicU32,
    /// Master -> core: "park yourself" request for a physical offline.
    /// 0 = none, 1 = requested, 2 = claimed by the core.
    pub park_request: AtomicU32,
    /// Core -> master: "I am parked and hold no lock" (safe to INIT now).
    pub parked: AtomicU32,
}

/// Park-protocol states (see `smp_cpu_offline` / `energy_cstate_idle`).
pub const PARK_NONE: u32 = 0;
pub const PARK_REQ: u32 = 1;
pub const PARK_CLAIMED: u32 = 2;

impl CpuSched {
    const fn new() -> Self {
        Self {
            current: ptr::null_mut(),
            idle: ptr::null_mut(),
            fpu_owner: ptr::null_mut(),
            schedule_in_progress: AtomicU32::new(0),
            park_request: AtomicU32::new(PARK_NONE),
            parked: AtomicU32::new(0),
        }
    }
}

// SAFETY: `CpuSched` holds raw `TaskStruct` pointers that are only ever read or written by the
// CPU owning the slot (indexed through the loaded TR), so sharing the table across CPUs cannot
// produce an unsynchronised access to any one slot.
unsafe impl Sync for CpuSched {}

static CPU_SCHED: SyncUnsafeCell<[CpuSched; MAX_CPUS]> =
    SyncUnsafeCell::new([const { CpuSched::new() }; MAX_CPUS]);

/// Index of the calling CPU, clamped into `[0, MAX_CPUS)`.
#[inline]
pub fn this_cpu() -> usize {
    let c = crate::smp::smp_self_cpu();
    if c > 0 && (c as usize) < MAX_CPUS {
        c as usize
    } else {
        0
    }
}

/// Pointer to `cpu`'s state.  Callers must pass `cpu < MAX_CPUS`.
#[inline]
fn slot(cpu: usize) -> *mut CpuSched {
    // SAFETY: callers clamp `cpu` into `[0, MAX_CPUS)`, so the offset stays inside the
    // statically allocated `CPU_SCHED` array.
    unsafe { (CPU_SCHED.get() as *mut CpuSched).add(cpu) }
}

/// This CPU's running task.
#[inline]
pub fn current() -> *mut TaskStruct {
    // SAFETY: `slot` returns a pointer inside the live `CPU_SCHED` array.
    unsafe { (*slot(this_cpu())).current }
}

/// Set this CPU's running task.
#[inline]
pub fn set_current(t: *mut TaskStruct) {
    // SAFETY: `slot` returns a pointer inside the live `CPU_SCHED` array; the store touches
    // only this CPU's slot.
    unsafe { (*slot(this_cpu())).current = t };
}

/// `cpu`'s running task (used by bring-up code that targets another CPU).
#[inline]
pub fn cpu_current(cpu: usize) -> *mut TaskStruct {
    // SAFETY: callers pass `cpu < MAX_CPUS`, so `slot` addresses a live table entry.
    unsafe { (*slot(cpu)).current }
}

/// `cpu`'s idle task (used to tell whether that core is idle).
#[inline]
pub fn cpu_idle(cpu: usize) -> *mut TaskStruct {
    // SAFETY: callers pass `cpu < MAX_CPUS`, so `slot` addresses a live table entry.
    unsafe { (*slot(cpu)).idle }
}

/// This CPU claims a pending park request (1 -> 2).  Returns true when it won
/// the claim, i.e. the master did not cancel it: the caller must then park.
#[inline]
pub fn claim_park_request() -> bool {
    // SAFETY: `slot` returns a pointer inside the live `CPU_SCHED` array; the
    // atomic lives there for the program's lifetime.
    let r = unsafe { &(*slot(this_cpu())).park_request };
    r.compare_exchange(PARK_REQ, PARK_CLAIMED, Ordering::Acquire, Ordering::Relaxed)
        .is_ok()
}

/// Record that this CPU is parked (release so the master's acquire sees it).
#[inline]
pub fn set_parked() {
    // SAFETY: `slot` returns a pointer inside the live `CPU_SCHED` array.
    unsafe { (*slot(this_cpu())).parked.store(1, Ordering::Release) };
}

/// Ask `cpu` to park itself.
#[inline]
pub fn set_park_request(cpu: usize) {
    // SAFETY: callers pass `cpu < MAX_CPUS`, so `slot` addresses a live entry.
    unsafe { (*slot(cpu)).park_request.store(PARK_REQ, Ordering::Relaxed) };
}

/// `cpu`'s parked flag.
#[inline]
pub fn cpu_parked(cpu: usize) -> u32 {
    // SAFETY: callers pass `cpu < MAX_CPUS`, so `slot` addresses a live entry.
    unsafe { (*slot(cpu)).parked.load(Ordering::Acquire) }
}

/// Cancel a still-unclaimed park request (1 -> 0).  Returns true when cancelled,
/// i.e. the core never claimed it and is safe to leave running.
#[inline]
pub fn cancel_park_request(cpu: usize) -> bool {
    // SAFETY: callers pass `cpu < MAX_CPUS`, so `slot` addresses a live entry.
    let r = unsafe { &(*slot(cpu)).park_request };
    r.compare_exchange(PARK_REQ, PARK_NONE, Ordering::Relaxed, Ordering::Relaxed)
        .is_ok()
}

/// Clear a CPU's park request and parked flags (used on bring-up / after INIT).
#[inline]
pub fn clear_park_state(cpu: usize) {
    // SAFETY: callers pass `cpu < MAX_CPUS`, so this touches only that CPU's slot.
    unsafe { (*slot(cpu)).park_request.store(PARK_NONE, Ordering::Relaxed) };
    // SAFETY: as above.
    unsafe { (*slot(cpu)).parked.store(0, Ordering::Relaxed) };
}

/// This CPU's idle task.
#[inline]
pub fn idle() -> *mut TaskStruct {
    // SAFETY: `slot` returns a pointer inside the live `CPU_SCHED` array.
    unsafe { (*slot(this_cpu())).idle }
}

/// Record `cpu`'s idle task and make it that CPU's current context.
#[inline]
pub fn set_idle_for(cpu: usize, t: *mut TaskStruct) {
    // SAFETY: callers pass `cpu < MAX_CPUS`, so the two stores below target a live table
    // entry belonging to that CPU only.
    unsafe {
        (*slot(cpu)).idle = t;
    }
}

/// This CPU's `schedule()` re-entry guard.
#[inline]
pub fn schedule_guard() -> &'static AtomicU32 {
    // SAFETY: `slot` addresses a live `CPU_SCHED` entry whose lifetime is the whole program;
    // the returned reference aliases only this CPU's own guard, which is the documented
    // single-owner contract of the guard.
    unsafe { &(*slot(this_cpu())).schedule_in_progress }
}

/// Address of this CPU's `current_task` slot, backing the C `current_task` macro.
#[no_mangle]
pub extern "C" fn cact_current_task_slot() -> *mut *mut TaskStruct {
    // SAFETY: `slot` addresses a live `CPU_SCHED` entry; the field's address is stable for the
    // program's lifetime, and only this CPU stores through it.
    unsafe { &mut (*slot(this_cpu())).current }
}

/// Value of this CPU's `current_task`, for foreign crates (`rust_mm`, `cact_sync`).
#[no_mangle]
pub extern "C" fn cact_current_task_get() -> *mut TaskStruct {
    current()
}

/// Address of this CPU's `fpu_owner` slot, backing the C `fpu_owner` macro.
#[no_mangle]
pub extern "C" fn cact_fpu_owner_slot() -> *mut *mut TaskStruct {
    // SAFETY: as for `cact_current_task_slot`, for the `fpu_owner` field.
    unsafe { &mut (*slot(this_cpu())).fpu_owner }
}
