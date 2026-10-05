//! Pure energy-governor model: C-state ids, the published energy costs, and the
//! benefit/cost decision functions.
//!
//! This module has **no hardware access, no globals and no FFI**, so it is the
//! single source of truth for the model and is unit-tested on the host by
//! `Kernel-Unit-Tests-for-Cact/` (P2.1).  The kernel's `cstate`, `monitor` and
//! `decision` modules are thin wrappers over the items here; nothing in this
//! file may reference `crate::` or call out.
//!
//! Model (Step 4 of the energy governor plan):
//!
//! ```text
//! benefit = (estimated_cycles * IPL * frequency) / power_per_cycle
//! cost    = wakeup_energy + cache_harm_energy
//! wakeup  <=> benefit > cost * 1.5
//! ```
//!
//! Policy thresholds:
//!   * load < 40%  AND idle >= 100 ms -> request deeper sleep (C3/C6)
//!   * load > 70%                     -> wake to C0
//!   * trend up    AND load > 70%     -> proactive wakeup

// C-state ids (must match `Cact/kernel/energy/energy.h`).
pub const CSTATE_C0: u32 = 0;
pub const CSTATE_C1: u32 = 1;
pub const CSTATE_C3: u32 = 2;
pub const CSTATE_C6: u32 = 3;

// Load trend (re-exported by `monitor`).
pub const TREND_DOWN: i32 = -1;
pub const TREND_STABLE: i32 = 0;
pub const TREND_UP: i32 = 1;

// Published per-state energy characteristics for a VM (relative energy units).
// Real values would come from ACPI _CST latency + RAPL/MSR calibration; these
// keep the benefit/cost model monotonic until a calibration driver exists.
pub const fn wakeup_energy(state: u32) -> u32 {
    match state {
        CSTATE_C1 => 5,
        CSTATE_C3 => 40,
        CSTATE_C6 => 160,
        _ => 0, // C0 (or out of range)
    }
}

pub const fn cache_harm_energy(state: u32) -> u32 {
    match state {
        CSTATE_C1 => 1,
        CSTATE_C3 => 30,
        CSTATE_C6 => 100,
        _ => 0,
    }
}

// Nominal CPU frequency used by the benefit model (Hz).
const NOMINAL_FREQ_HZ: u64 = 3_000_000_000;
// Average estimated cycles of one runnable task burst.
const AVG_TASK_CYCLES: u64 = 1_000_000;
// Energy per cycle divisor so benefit lands in the same units as the wakeup and
// cache-harm costs.  With AVG_TASK_CYCLES = 1e6 and FREQ = 3e9 this makes
// benefit ~= queue_len * IPL.
const POWER_PER_CYCLE: u64 = 3_000_000_000_000_000;

// benefit > cost * 1.5  <=>  benefit * 2 > cost * 3
const COST_MARGIN_NUM: u64 = 3;
const COST_MARGIN_DEN: u64 = 2;

const LOAD_SLEEP_PERMILLE: u32 = 400; // < 40%
const LOAD_WAKE_PERMILLE: u32 = 700;  // > 70%
const IDLE_SLEEP_MS: u32 = 100;

/// Estimated cycles of the work queued for a core.  IPL 0 is treated as 1 so a
/// caller that does not know the interrupt priority still gets a usable value.
pub fn work_cycles(queue_len: u32, ipl: u32) -> u64 {
    let ipl = if ipl == 0 { 1 } else { ipl };
    (queue_len as u64)
        .saturating_mul(AVG_TASK_CYCLES)
        .saturating_mul(ipl as u64)
}

/// Benefit of waking a core to drain `queue_len` tasks (plan formula, scaled).
pub fn benefit(queue_len: u32, ipl: u32) -> u64 {
    let cycles = work_cycles(queue_len, ipl);
    let benefit = (cycles as u128)
        .saturating_mul(NOMINAL_FREQ_HZ as u128)
        .checked_div(POWER_PER_CYCLE as u128)
        .unwrap_or(0);
    benefit.min(u64::MAX as u128) as u64
}

/// Energy cost of a transition into `state`: wakeup + cache-harm.
pub fn cost(state: u32) -> u64 {
    (wakeup_energy(state) as u64).saturating_add(cache_harm_energy(state) as u64)
}

/// Should a core currently in `state` be woken?  Applies the benefit/cost
/// formula and the load/trend rules.
pub fn should_wake(state: u32, queue_len: u32, ipl: u32, load_permille: u32, trend: i32) -> i32 {
    if state == CSTATE_C0 {
        return 0;
    }
    // Formula: pending work is worth waking for.
    if queue_len > 0 {
        let b = benefit(queue_len, ipl);
        let c = cost(state);
        if b.saturating_mul(COST_MARGIN_DEN) > c.saturating_mul(COST_MARGIN_NUM) {
            return 1;
        }
    }
    // Overload and proactive (rising load) rules.
    if load_permille > LOAD_WAKE_PERMILLE && trend == TREND_UP {
        return 1;
    }
    if load_permille > LOAD_WAKE_PERMILLE {
        return 1;
    }
    0
}

/// Should an idle core be pushed into a deeper C-state?
pub fn should_sleep(load_permille: u32, idle_ms: u32) -> i32 {
    if load_permille < LOAD_SLEEP_PERMILLE && idle_ms >= IDLE_SLEEP_MS {
        1
    } else {
        0
    }
}
