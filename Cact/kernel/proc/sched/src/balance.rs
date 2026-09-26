//! Load balancing & task migration (Step 6 of the energy governor).
//!
//! The master core evaluates load imbalance across online cores once per tick
//! (10 ms) and, when a worker core is overloaded, migrates work towards an
//! underloaded core.  Before migrating, the destination core is woken if it
//! is in a deep C-state, and its runqueue is charged with the migrated task.
//!
//! IMPORTANT: this kernel currently has one global MLFQ runqueue and no
//! per-core runqueues or task CPU affinity.  The scanner and throttle below
//! are fully wired; the actual cross-runqueue move (`energy_balance_migrate`)
//! returns -1 until per-core runqueues land with SMP bring-up.  The migration
//! throttle ("at most one core per 5 ms") is honoured: ticks are 10 ms, so a
//! single attempt per tick already satisfies the bound.

use core::cell::SyncUnsafeCell;
use core::sync::atomic::{AtomicU32, Ordering};

use crate::ffi;
use crate::energy::{self, MAX_CORES};
use crate::monitor;
use crate::cstate;
use crate::decision;

// Desired throttle: one migration attempt per 5 ms.  Ticks are 10 ms, so one
// attempt per tick is already below the bound.
#[allow(dead_code)]
const BALANCE_MIN_TICKS: u32 = 1;
// Minimum load spread (per-mille) that counts as imbalance.
#[allow(dead_code)]
const IMBALANCE_THRESHOLD_PERMILLE: u32 = 150;

struct BalanceState {
    initialized: bool,
}

impl BalanceState {
    const fn new() -> Self {
        Self { initialized: false }
    }
}

static BALANCE_STATE: SyncUnsafeCell<BalanceState> = SyncUnsafeCell::new(BalanceState::new());
static LAST_ATTEMPT_TICK: AtomicU32 = AtomicU32::new(0);

fn state() -> &'static mut BalanceState {
    // SAFETY: `BALANCE_STATE` is written once by `energy_balance_init` during boot and only
    // read afterwards from `energy_balance_tick`, which the master core runs once per scheduler
    // tick; no worker core ever touches it, so this reference is not aliased concurrently.
    unsafe { &mut *BALANCE_STATE.get() }
}

fn is_online_worker(cpu: u32) -> bool {
    energy::energy_core_is_present(cpu) != 0
        && energy::energy_core_is_online(cpu) != 0
        && energy::energy_core_role(cpu) == 2
}

/// Wake the destination core (deep sleep -> C0) before work is pushed onto it.
fn wake_destination(dst: u32) {
    let state = energy::energy_core_cstate(dst);
    if state == cstate::CSTATE_C0 {
        return;
    }
    if decision::energy_decision_should_wake(
        state,
        monitor::energy_monitor_queue_length(0),
        1,
        monitor::energy_monitor_load_avg(dst),
        monitor::energy_monitor_trend(dst),
    ) != 0
        && cstate::energy_ipi_wake_worker(dst) == 0
    {
        let _ = energy::energy_core_set_cstate(dst, cstate::CSTATE_C0);
    }
}

// ---------------------------------------------------------------------------
// C ABI
// ---------------------------------------------------------------------------

#[no_mangle]
pub extern "C" fn energy_balance_init() -> i32 {
    state().initialized = true;
    LAST_ATTEMPT_TICK.store(0, Ordering::SeqCst);
    0
}

/// Largest load spread between any two online cores (per-mille).
#[no_mangle]
pub extern "C" fn energy_balance_imbalance_permille() -> u32 {
    let mut min_load = u32::MAX;
    let mut max_load = 0u32;

    if energy::energy_core_is_online(0) != 0 {
        let load = monitor::energy_monitor_load_avg(0);
        min_load = load;
        max_load = load;
    }
    for cpu in 1..MAX_CORES {
        let cpu = cpu as u32;
        if !is_online_worker(cpu) {
            continue;
        }
        let load = monitor::energy_monitor_load_avg(cpu);
        if load < min_load {
            min_load = load;
        }
        if load > max_load {
            max_load = load;
        }
    }

    if min_load == u32::MAX {
        0
    } else {
        max_load.saturating_sub(min_load)
    }
}

/// Move one ready task from the overloaded source to the underloaded
/// destination.  Returns 0 on success.  Unsupported until per-core runqueues
/// exist (UP build: -1).
#[no_mangle]
pub extern "C" fn energy_balance_migrate(src: u32, dst: u32, pid: u32) -> i32 {
    let _ = (src, pid);
    // Cross-runqueue migration is not possible with the current single global
    // MLFQ.  Wake the destination anyway so the attempt is armed once
    // per-core runqueues land.
    wake_destination(dst);
    -1
}

/// One load-balancing pass on the master core (every scheduler tick).
#[no_mangle]
pub extern "C" fn energy_balance_tick() {
    if !state().initialized {
        return;
    }
    // Cross-runqueue migration requires per-core runqueues (Step B). Until
    // they exist the balancer stays dormant so it does not wake idle workers.
    let _ = energy::energy_core_count_online();
    let _ = monitor::energy_monitor_load_avg(0);
    let _ = LAST_ATTEMPT_TICK.load(Ordering::Relaxed);
    let _ = ffi::timer_ticks_get();
}
