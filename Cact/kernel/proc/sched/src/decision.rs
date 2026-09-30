//! Decision engine (Step 4 of the energy governor).
//!
//! Runs periodically on the master core and decides, per worker core, whether
//! to let it sleep deeper, keep it halted, or wake it back to C0.  A wakeup is
//! only issued when the expected benefit of executing pending work outweighs
//! the transition cost (plan formula):
//!
//! ```text
//! benefit = (estimated_cycles * IPL * frequency) / power_per_cycle
//! cost    = wakeup_energy + cache_harm_energy
//! wakeup  <=> benefit > cost * 1.5
//! ```
//!
//! Policy thresholds from the plan:
//!   * load < 40%  AND idle > 100 ms  -> request deeper sleep (C3/C6)
//!   * load > 70%  AND idle > 10 ms   -> wake to C0
//!   * trend up    AND load > 70%     -> proactive wakeup
//!
//! Cadence: the plan targets one evaluation every 5 ms.  The system tick here
//! is 100 Hz (10 ms), so the engine evaluates once per scheduler tick on the
//! master core (`energy_decision_tick`).

use crate::ffi;
use crate::energy::{self, MAX_CORES};
use crate::monitor;
use crate::cstate;

use core::sync::atomic::{AtomicBool, Ordering};

pub const CSTATE_C0: u32 = cstate::CSTATE_C0;
pub const CSTATE_C1: u32 = cstate::CSTATE_C1;
pub const CSTATE_C3: u32 = cstate::CSTATE_C3;
pub const CSTATE_C6: u32 = cstate::CSTATE_C6;

// Nominal CPU frequency used by the benefit model (Hz).
const NOMINAL_FREQ_HZ: u64 = 3_000_000_000;
// Average estimated cycles of one runnable task burst.
const AVG_TASK_CYCLES: u64 = 1_000_000;
// Energy per cycle divisor so benefit lands in the same units as the
// wakeup/cache-harm costs reported by the C-state descriptors.  With
// AVG_TASK_CYCLES = 1e6 and FREQ = 3e9 this makes benefit ~= queue_len*IPL.
const POWER_PER_CYCLE: u64 = 3_000_000_000_000_000;

// benefit > cost * 1.5  <=>  benefit * 2 > cost * 3
const COST_MARGIN_NUM: u64 = 3;
const COST_MARGIN_DEN: u64 = 2;

const LOAD_SLEEP_PERMILLE: u32 = 400; // < 40%
const LOAD_WAKE_PERMILLE: u32 = 700;  // > 70%
const IDLE_SLEEP_MS: u32 = 100;
const IDLE_WAKE_MS: u32 = 10;

const TICK_MS: u32 = 10;

// ---------------------------------------------------------------------------
// Benefit / cost model
// ---------------------------------------------------------------------------

/// Estimated cycles of the work queued for a core.
#[no_mangle]
pub extern "C" fn energy_decision_work_cycles(queue_len: u32, ipl: u32) -> u64 {
    let ipl = if ipl == 0 { 1 } else { ipl };
    (queue_len as u64).saturating_mul(AVG_TASK_CYCLES).saturating_mul(ipl as u64)
}

/// Benefit of waking a core to drain `queue_len` tasks (plan formula, scaled).
#[no_mangle]
pub extern "C" fn energy_decision_benefit(queue_len: u32, ipl: u32) -> u64 {
    let cycles = energy_decision_work_cycles(queue_len, ipl);
    let benefit = (cycles as u128)
        .saturating_mul(NOMINAL_FREQ_HZ as u128)
        .checked_div(POWER_PER_CYCLE as u128)
        .unwrap_or(0);
    benefit.min(u64::MAX as u128) as u64
}

/// Energy cost of a transition into `state`: wakeup + cache-harm.
#[no_mangle]
pub extern "C" fn energy_decision_cost(state: u32) -> u64 {
    (cstate::energy_cstate_wakeup_energy(state) as u64)
        .saturating_add(cstate::energy_cstate_cache_harm_energy(state) as u64)
}

/// Should a core currently in `state` be woken?  Applies the plan's
/// benefit/cost formula and the trend-based proactive rule.
#[no_mangle]
pub extern "C" fn energy_decision_should_wake(
    state: u32,
    queue_len: u32,
    ipl: u32,
    load_permille: u32,
    trend: i32,
) -> i32 {
    if state == CSTATE_C0 {
        return 0;
    }
    // Formula: pending work is worth waking for.
    if queue_len > 0 {
        let benefit = energy_decision_benefit(queue_len, ipl);
        let cost = energy_decision_cost(state);
        if benefit.saturating_mul(COST_MARGIN_DEN)
            > cost.saturating_mul(COST_MARGIN_NUM)
        {
            return 1;
        }
    }
    // Overload and proactive (rising load) rules.
    if load_permille > LOAD_WAKE_PERMILLE && trend == monitor::TREND_UP {
        return 1;
    }
    if load_permille > LOAD_WAKE_PERMILLE {
        return 1;
    }
    0
}

/// Should an idle core be pushed into a deeper C-state?
#[no_mangle]
pub extern "C" fn energy_decision_should_sleep(load_permille: u32, idle_ms: u32) -> i32 {
    if load_permille < LOAD_SLEEP_PERMILLE && idle_ms >= IDLE_SLEEP_MS {
        1
    } else {
        0
    }
}

fn idle_ms_of(cpu: u32) -> u32 {
    let now = ffi::timer_ticks_get();
    let since = energy::energy_core_idle_since_tick(cpu);
    let elapsed = now.wrapping_sub(since);
    // Only meaningful once the core is recorded idle; otherwise 0.
    if energy::energy_core_is_idle(cpu) == 0 {
        return 0;
    }
    elapsed.saturating_mul(TICK_MS)
}

// ---------------------------------------------------------------------------
// Periodic evaluation on the master core
// ---------------------------------------------------------------------------

/// One evaluation pass. Called from the scheduler tick on the master core.
/// Scans online worker cores and issues IPI_HALT / IPI_WAKEUP.
#[no_mangle]
pub extern "C" fn energy_decision_tick() {
    // Only a multi-core configuration has workers to drive.
    if energy::energy_core_count_online() <= 1 {
        return;
    }

    for cpu in 1..MAX_CORES {
        let cpu = cpu as u32;
        if energy::energy_core_is_present(cpu) == 0 || energy::energy_core_is_online(cpu) == 0 {
            continue;
        }
        if energy::energy_core_role(cpu) != 2 {
            continue; // workers only
        }

        let state = energy::energy_core_cstate(cpu);
        let load = monitor::energy_monitor_load_avg(cpu);
        let trend = monitor::energy_monitor_trend(cpu);
        let idle_ms = idle_ms_of(cpu);
        // Until per-core runqueues exist, pending global work is a reasonable
        // proxy for what a woken worker could drain.
        let queue_len = monitor::energy_monitor_queue_length(0);

        if state == CSTATE_C0 {
            if energy_decision_should_sleep(load, idle_ms) != 0
                && cstate::energy_cstate_enter(cpu, CSTATE_C1) == 0
            {
                let _ = cstate::energy_ipi_halt_worker(cpu);
            }
            continue;
        }

        // Core is halted/asleep: decide whether to wake it.
        let wake = energy_decision_should_wake(state, queue_len, 1, load, trend);
        if wake != 0 {
            if idle_ms >= IDLE_WAKE_MS && cstate::energy_ipi_wake_worker(cpu) == 0 {
                let _ = energy::energy_core_set_cstate(cpu, CSTATE_C0);
                let cost = energy_decision_cost(state) as u32;
                monitor::energy_monitor_charge_energy(cpu, cost);
            }
            continue;
        }

        // No reason to wake; request an even deeper sleep if it pays off.
        if energy_decision_should_sleep(load, idle_ms) != 0 {
            // Only a real deeper state (C3/C6 via _CST/MWAIT) makes IPI_HALT
            // useful; with C1 as the deepest executable state the core is
            // already where it should be.
            let deeper_available = cstate::energy_cstate_available(CSTATE_C3) != 0
                || cstate::energy_cstate_available(CSTATE_C6) != 0;
            if deeper_available {
                let _ = cstate::energy_ipi_halt_worker(cpu);
            }
        }
    }

    // Worker-initiated help: a saturated worker that still saw queued work asked
    // for a peer.  Consume the requests and wake one sleeping worker so the
    // queue drains on two cores (subject to the same idle/benefit rules).
    let mut help = false;
    for cpu in 1..MAX_CORES {
        if monitor::energy_monitor_take_help(cpu as u32) != 0 {
            help = true;
        }
    }
    if help && monitor::energy_monitor_queue_length(0) > 0 {
        for cpu in 1..MAX_CORES {
            let cpu = cpu as u32;
            if energy::energy_core_is_present(cpu) == 0
                || energy::energy_core_is_online(cpu) == 0
                || energy::energy_core_role(cpu) != 2
            {
                continue;
            }
            let state = energy::energy_core_cstate(cpu);
            if state == CSTATE_C0 || idle_ms_of(cpu) < IDLE_WAKE_MS {
                continue;
            }
            if cstate::energy_ipi_wake_worker(cpu) == 0 {
                let _ = energy::energy_core_set_cstate(cpu, CSTATE_C0);
                monitor::energy_monitor_charge_energy(cpu, energy_decision_cost(state) as u32);
            }
            break; // one helper per tick
        }
    }
}

#[no_mangle]
pub extern "C" fn energy_decision_init() -> i32 {
    0
}

// ---------------------------------------------------------------------------
// Physical core offlining / onlining (the slow tier)
// ---------------------------------------------------------------------------

/// Physical offlining/onlining of workers: a long-idle worker is parked and
/// INIT'd, and wakes again through the boot trampoline when work appears.
///
/// Two bugs in that path caused the reported "`cat /proc/cpuinfo` freezes QEMU"
/// (2026-09-30, QEMU, 4 vCPUs): the guest requested a machine reset while the
/// command's output was on the console, and QEMU — started with `-no-reboot
/// -no-shutdown` — stops the VM instead of rebooting, so the output froze
/// mid-line.
///   1. `smp::stack_top` rounded the stack top *up*, past the end of
///      `idle_stack`, so the first two pushes of every bring-up landed on
///      `lapic_id`/`online`.  Every worker's recorded LAPIC id became garbage and
///      the re-online sent its INIT to that value — APIC id 0, the BSP, which
///      resets the machine.
///   2. A re-onlined core re-ran `ltr` with the TSS descriptor still marked busy
///      by the first bring-up.  The #GP that follows happens before the AP has
///      an IDT, so it becomes a triple fault.
/// Both are fixed (`smp.rs`), and the IPI helpers now refuse a destination that
/// is the BSP's own id or the invalid sentinel, so a bad core-map entry cannot
/// take the machine down either.
///
/// Kill switch: `energy_core_offline_enable(0)`.
static ALLOW_CORE_OFFLINE: AtomicBool = AtomicBool::new(true);

/// Park a worker after it has been idle at least this long.
const OFFLINE_IDLE_MS: u32 = 3000;
/// Never offline below this many online cores (master + one worker).
const MIN_ONLINE_CORES: u32 = 2;

/// Enable/disable physical offlining (on by default; 0 is the kill switch).
#[no_mangle]
pub extern "C" fn energy_core_offline_enable(on: i32) {
    ALLOW_CORE_OFFLINE.store(on != 0, Ordering::Relaxed);
}

/// Run from the master's idle loop (task context — the online sequence must not
/// block the timer ISR).  Brings an offlined worker back when queued work has
/// no online core free, otherwise parks a long-idle worker while capacity is in
/// excess.  At most one transition per pass.
#[no_mangle]
pub extern "C" fn energy_core_manage() {
    if !ALLOW_CORE_OFFLINE.load(Ordering::Relaxed) {
        return;
    }

    // Bring one offlined worker back when there is work and every online worker
    // is busy (C0); a parked core is otherwise invisible to the tick's scan.
    if monitor::energy_monitor_queue_length(0) != 0 {
        let mut any_offline = false;
        let mut online_workers = 0u32;
        let mut busy_online_workers = 0u32;
        for cpu in 1..MAX_CORES {
            let cpu = cpu as u32;
            if energy::energy_core_is_present(cpu) == 0 || energy::energy_core_role(cpu) != 2 {
                continue;
            }
            if energy::energy_core_is_online(cpu) == 0 {
                any_offline = true;
            } else {
                online_workers += 1;
                if energy::energy_core_cstate(cpu) == CSTATE_C0 {
                    busy_online_workers += 1;
                }
            }
        }
        if any_offline && online_workers > 0 && busy_online_workers == online_workers {
            for cpu in 1..MAX_CORES {
                let cpu = cpu as u32;
                if energy::energy_core_is_present(cpu) != 0
                    && energy::energy_core_role(cpu) == 2
                    && energy::energy_core_is_online(cpu) == 0
                    && crate::smp::smp_cpu_online_sipi(cpu) == 0
                {
                    return;
                }
            }
        }
    }

    // Park a long-idle worker while nothing is queued and capacity is in excess.
    if energy::energy_core_count_online() <= MIN_ONLINE_CORES
        || monitor::energy_monitor_queue_length(0) != 0
    {
        return;
    }
    for cpu in 1..MAX_CORES {
        let cpu = cpu as u32;
        if energy::energy_core_is_present(cpu) == 0
            || energy::energy_core_is_online(cpu) == 0
            || energy::energy_core_role(cpu) != 2
            || energy::energy_core_is_idle(cpu) == 0
        {
            continue;
        }
        if idle_ms_of(cpu) < OFFLINE_IDLE_MS {
            continue;
        }
        let _ = crate::smp::smp_cpu_offline(cpu);
        return;
    }
}
