//! Load balancing & task migration (Step 6 of the energy governor).
//!
//! The scheduler keeps a ready queue per logical core, so a task is made
//! runnable *on a core* rather than in one global pool.  Two things move it
//! between cores:
//!
//!   * an idle core *pulls* work on its own (`pick_next_task`'s steal step), and
//!   * this module *pushes*: once per tick the master core compares the online
//!     cores' load and, when one is clearly busier than another, moves one ready
//!     task from the busy core's runqueue to the idle one, waking the
//!     destination first when it sits in a deep C-state.
//!
//! The push path is what the pull path cannot do: it can relieve a core that is
//! busy while its peers are only *busier than it is* (no free core left to
//! steal), and it acts immediately instead of at the victim's next tick.
//!
//! `balance_policy` owns the core-selection rule (pure, host-tested); this
//! module owns the runqueue side and the wake.

use core::cell::SyncUnsafeCell;
use core::sync::atomic::{AtomicU32, Ordering};

use crate::balance_policy::{self, CoreLoad};
use crate::ffi;
use crate::energy::{self, MAX_CORES};
use crate::mlfq;
use crate::monitor;
use crate::cstate;
use crate::decision;
use crate::sync::{irq_spinlock_acquire, irq_spinlock_release};

// Desired throttle: one migration attempt per 5 ms.  Ticks are 10 ms, so a
// bound of one attempt per tick already satisfies it.
const BALANCE_MIN_TICKS: u32 = 1;
// Minimum load spread (per-mille) that counts as imbalance.
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

/// Snapshot every core for the balancer: onlineness, smoothed load, and the
/// number of tasks waiting on that core's own ready queue.
///
/// The runnable counts are read without `SCHEDULER_LOCK`; they only feed the
/// choice, and the move re-validates under the lock, so a stale count costs at
/// worst one wasted attempt.
fn core_loads() -> [CoreLoad; MAX_CORES] {
    let mut out = [CoreLoad::empty(0); MAX_CORES];
    for cpu in 0..MAX_CORES as u32 {
        out[cpu as usize] = CoreLoad {
            cpu,
            online: energy::energy_core_is_online(cpu) != 0,
            load_permille: monitor::energy_monitor_load_avg(cpu),
            runnable: mlfq::mlfq_local_runnable_count(cpu as usize),
        };
    }
    out
}

/// Wake the destination core (deep sleep -> C0) before work is pushed onto it.
fn wake_destination(dst: u32) {
    let state = energy::energy_core_cstate(dst);
    if state == cstate::CSTATE_C0 {
        return;
    }
    // The migrated task is already on the destination's queue, so it counts.
    let pending = mlfq::mlfq_local_runnable_count(dst as usize) + 1;
    if decision::energy_decision_should_wake(
        state,
        pending,
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

/// Move one ready task from `src`'s runqueue to `dst`'s and wake the
/// destination if it is asleep.  `pid == 0` moves whatever `src` has queued
/// first (the balancer's case); a non-zero `pid` moves that task and fails
/// unless it is ready on `src`.  Returns 0 on success, -1 otherwise.
#[no_mangle]
pub extern "C" fn energy_balance_migrate(src: u32, dst: u32, pid: u32) -> i32 {
    if src == dst
        || src as usize >= MAX_CORES
        || dst as usize >= MAX_CORES
        || energy::energy_core_is_online(dst) == 0
    {
        return -1;
    }

    // SAFETY: `SCHEDULER_LOCK` is the scheduler's global spinlock; this runs on
    // the master's tick/idle path, which does not hold it.
    unsafe { irq_spinlock_acquire(&raw mut crate::task::SCHEDULER_LOCK) };
    // SAFETY: the lock is held, so `src`'s ready queues are stable for the move.
    let moved = unsafe { mlfq::mlfq_migrate_locked(src, dst, pid) };
    // SAFETY: pairs with the acquire above.
    unsafe { irq_spinlock_release(&raw mut crate::task::SCHEDULER_LOCK) };

    if moved == 0 {
        return -1;
    }

    // Real work landed on `dst`: pull it out of a deep idle state now rather
    // than letting the task wait for the destination's next idle-timer tick.
    wake_destination(dst);
    0
}

/// One load-balancing pass on the master core (every scheduler tick).
#[no_mangle]
pub extern "C" fn energy_balance_tick() {
    if !state().initialized {
        return;
    }
    // A single online core has nothing to balance against.
    if energy::energy_core_count_online() <= 1 {
        return;
    }
    // Nothing to move anywhere.
    if mlfq::mlfq_runnable_count() == 0 {
        return;
    }

    let now = ffi::timer_ticks_get();
    if now.wrapping_sub(LAST_ATTEMPT_TICK.load(Ordering::Relaxed)) < BALANCE_MIN_TICKS {
        return;
    }

    let loads = core_loads();
    let threshold = IMBALANCE_THRESHOLD_PERMILLE;
    let Some((src, dst)) = balance_policy::pick_migration(&loads, threshold) else {
        return;
    };

    // `src` has runnable work and is the busiest core; hand one task to `dst`,
    // the least-loaded one.
    if energy_balance_migrate(src, dst, 0) == 0 {
        LAST_ATTEMPT_TICK.store(now, Ordering::Relaxed);
    }
}
