//! Load monitoring (Step 3 of the energy governor).
//!
//! The master core samples, on every scheduler tick, per-core runnable load
//! and idleness and folds them into exponentially-smoothed metrics:
//! `load_avg`, `queue_length`, `cpu_utilization`, a `trend` classifier and a
//! per-second `energy_budget`.  The plan targets a 1 ms cadence; this kernel's
//! system tick is 100 Hz (10 ms), so each tick is one sample and the second
//! window is 100 samples.  When a sub-10 ms master timer appears, only the
//! window constant needs to change.
//!
//! On this UP port all samples describe the single online core (cpu0 = master)
//! and are taken from the global MLFQ runqueue.  Per-core accounting for
//! worker cores will fill the same slots once per-core runqueues exist.

use core::cell::SyncUnsafeCell;

use crate::energy::MAX_CORES;
use crate::mlfq;
use crate::task::current_task;
use crate::sync::{irq_spinlock_acquire, irq_spinlock_release};

// Metric scale: 1000 = 100%.
pub const PERMILLE: u32 = 1000;

// Trend classifier values must match Cact/kernel/energy/energy.h.
pub const TREND_DOWN: i32 = -1;
pub const TREND_STABLE: i32 = 0;
pub const TREND_UP: i32 = 1;

// 100 Hz tick => 100 samples per second.
const SAMPLES_PER_SEC: u32 = 100;
// Exponential smoothing divisor (alpha = 1/8).
const EMA_DIV: u32 = 8;
const TREND_HYST_PERMILLE: u32 = 40;
const DEFAULT_ENERGY_BUDGET: u32 = 10000;

#[derive(Copy, Clone)]
struct CoreLoad {
    load_avg: u32,          // per-mille, EMA over samples
    prev_load_avg: u32,     // previous sample for trend detection
    queue_length: u32,      // last sampled runnable count
    cpu_utilization: u32,   // per-mille over the last 1 s window
    busy_samples: u32,      // samples where the core ran a task
    window_samples: u32,    // samples accumulated in the current window
    trend: i32,             // TREND_* classifier
    energy_budget: u32,     // reset to DEFAULT every second
    first_sample: bool,
}

impl CoreLoad {
    const fn fresh() -> Self {
        Self {
            load_avg: 0,
            prev_load_avg: 0,
            queue_length: 0,
            cpu_utilization: 0,
            busy_samples: 0,
            window_samples: 0,
            trend: TREND_STABLE,
            energy_budget: DEFAULT_ENERGY_BUDGET,
            first_sample: false,
        }
    }
}

struct MonitorState {
    cores: [CoreLoad; MAX_CORES],
    initialized: bool,
}

impl MonitorState {
    const fn new() -> Self {
        Self {
            cores: [CoreLoad::fresh(); MAX_CORES],
            initialized: false,
        }
    }
}

static MONITOR_STATE: SyncUnsafeCell<MonitorState> = SyncUnsafeCell::new(MonitorState::new());
static mut MONITOR_LOCK: crate::sync::irq_spinlock_t = crate::sync::irq_spinlock_t::new();

fn state() -> &'static mut MonitorState {
    unsafe { &mut *MONITOR_STATE.get() }
}

fn lock() {
    unsafe { irq_spinlock_acquire(&raw mut MONITOR_LOCK) };
}

fn unlock() {
    unsafe { irq_spinlock_release(&raw mut MONITOR_LOCK) };
}

fn core_index(cpu: u32) -> Option<usize> {
    if (cpu as usize) < MAX_CORES {
        Some(cpu as usize)
    } else {
        None
    }
}

fn query<T>(f: impl FnOnce(&MonitorState) -> T) -> T {
    lock();
    let st = state();
    let out = f(st);
    unlock();
    out
}

/// Advance the smoothed `load_avg` by one instantaneous sample.
fn update_load_avg(core: &mut CoreLoad, instant_permille: u32) {
    if core.first_sample {
        let delta = (instant_permille as i64 - core.load_avg as i64) / EMA_DIV as i64;
        core.load_avg = (core.load_avg as i64 + delta).clamp(0, PERMILLE as i64) as u32;
    } else {
        core.load_avg = instant_permille;
        core.first_sample = true;
    }
}

/// Classify the load trend from the last two smoothed samples.
fn update_trend(core: &mut CoreLoad) {
    let cur = core.load_avg;
    let prev = core.prev_load_avg;
    if cur > prev.saturating_add(TREND_HYST_PERMILLE) {
        core.trend = TREND_UP;
    } else if cur.saturating_add(TREND_HYST_PERMILLE) < prev {
        core.trend = TREND_DOWN;
    } else {
        core.trend = TREND_STABLE;
    }
    core.prev_load_avg = cur;
}

// ---------------------------------------------------------------------------
// Sampling (invoked from the scheduler tick on the master core)
// ---------------------------------------------------------------------------

/// Sample the master core. Runs on every scheduler tick with the scheduler
/// lock held and IRQs off.
pub(crate) fn sample_tick() {
    let pid = unsafe { (*current_task).pid };
    let busy = pid != 0;
    let queue_len = mlfq::mlfq_runnable_count();
    let instant = if busy { PERMILLE } else { 0 };

    lock();
    let st = state();
    if !st.initialized {
        unlock();
        return;
    }

    let core = &mut st.cores[0]; // master slot (cpu0)
    core.queue_length = queue_len;
    update_load_avg(core, instant);
    update_trend(core);

    if busy {
        core.busy_samples += 1;
    }
    core.window_samples += 1;
    if core.window_samples >= SAMPLES_PER_SEC {
        core.cpu_utilization = if core.window_samples != 0 {
            (core.busy_samples * PERMILLE) / core.window_samples
        } else {
            0
        };
        core.busy_samples = 0;
        core.window_samples = 0;
        core.energy_budget = DEFAULT_ENERGY_BUDGET;
    }

    unlock();
}

// ---------------------------------------------------------------------------
// C ABI
// ---------------------------------------------------------------------------

#[no_mangle]
pub extern "C" fn energy_monitor_init() -> i32 {
    lock();
    let st = state();
    for c in st.cores.iter_mut() {
        *c = CoreLoad::fresh();
    }
    st.initialized = true;
    unlock();
    0
}

#[no_mangle]
pub extern "C" fn energy_monitor_load_avg(cpu: u32) -> u32 {
    match core_index(cpu) {
        Some(i) => query(|st| st.cores[i].load_avg),
        None => 0,
    }
}

#[no_mangle]
pub extern "C" fn energy_monitor_queue_length(cpu: u32) -> u32 {
    match core_index(cpu) {
        Some(i) => query(|st| st.cores[i].queue_length),
        None => 0,
    }
}

#[no_mangle]
pub extern "C" fn energy_monitor_cpu_utilization(cpu: u32) -> u32 {
    match core_index(cpu) {
        Some(i) => query(|st| st.cores[i].cpu_utilization),
        None => 0,
    }
}

#[no_mangle]
pub extern "C" fn energy_monitor_trend(cpu: u32) -> i32 {
    match core_index(cpu) {
        Some(i) => query(|st| st.cores[i].trend),
        None => TREND_STABLE,
    }
}

#[no_mangle]
pub extern "C" fn energy_monitor_energy_budget(cpu: u32) -> u32 {
    match core_index(cpu) {
        Some(i) => query(|st| st.cores[i].energy_budget),
        None => 0,
    }
}

/// Deduct energy spent on a decision (e.g. a wakeup) from the core budget.
/// The budget refills to its per-second cap automatically on the window edge.
#[no_mangle]
pub extern "C" fn energy_monitor_charge_energy(cpu: u32, amount: u32) {
    let Some(idx) = core_index(cpu) else { return };
    lock();
    let core = &mut state().cores[idx];
    core.energy_budget = core.energy_budget.saturating_sub(amount);
    unlock();
}

#[no_mangle]
pub extern "C" fn energy_monitor_refill_budget(cpu: u32) {
    let Some(idx) = core_index(cpu) else { return };
    lock();
    state().cores[idx].energy_budget = DEFAULT_ENERGY_BUDGET;
    unlock();
}
