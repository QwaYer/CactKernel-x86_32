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
use crate::energy_model as model;
use crate::placement;

use core::sync::atomic::{AtomicBool, Ordering};

pub const CSTATE_C0: u32 = model::CSTATE_C0;
pub const CSTATE_C1: u32 = model::CSTATE_C1;
pub const CSTATE_C3: u32 = model::CSTATE_C3;
pub const CSTATE_C6: u32 = model::CSTATE_C6;

// The benefit/cost model and its constants live in `crate::energy_model` (pure,
// host-tested).  The thresholds below stay here because they are policy, not
// model, and are only used by the tick.
const IDLE_WAKE_MS: u32 = 10;

const TICK_MS: u32 = 10;

// ---------------------------------------------------------------------------
// Benefit / cost model (pure implementation: `crate::energy_model`)
// ---------------------------------------------------------------------------

/// Estimated cycles of the work queued for a core.
#[no_mangle]
pub extern "C" fn energy_decision_work_cycles(queue_len: u32, ipl: u32) -> u64 {
    model::work_cycles(queue_len, ipl)
}

/// Benefit of waking a core to drain `queue_len` tasks (plan formula, scaled).
#[no_mangle]
pub extern "C" fn energy_decision_benefit(queue_len: u32, ipl: u32) -> u64 {
    model::benefit(queue_len, ipl)
}

/// Energy cost of a transition into `state`: wakeup + cache-harm.
#[no_mangle]
pub extern "C" fn energy_decision_cost(state: u32) -> u64 {
    model::cost(state)
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
    model::should_wake(state, queue_len, ipl, load_permille, trend)
}

/// Should an idle core be pushed into a deeper C-state?
#[no_mangle]
pub extern "C" fn energy_decision_should_sleep(load_permille: u32, idle_ms: u32) -> i32 {
    model::should_sleep(load_permille, idle_ms)
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

/// Wake a sleeping worker and charge the transition cost.  Re-reads the state,
/// so callers need no snapshot.
fn wake_worker(cpu: u32) {
    let state = energy::energy_core_cstate(cpu);
    if state == CSTATE_C0 {
        return;
    }
    if cstate::energy_ipi_wake_worker(cpu) == 0 {
        let _ = energy::energy_core_set_cstate(cpu, CSTATE_C0);
        monitor::energy_monitor_charge_energy(cpu, energy_decision_cost(state) as u32);
    }
}

/// Wake the workers the tick marked in `want`.  Sibling-aware when SMT is
/// present: at most one logical CPU per physical core and fresh cores first, so
/// the second runnable thread does not join the master's SMT sibling while
/// other physical cores are idle.  without SMT the old per-worker wake is kept.
fn wake_selected(want: &[bool; MAX_CORES]) {
    if crate::cpu_topo::threads_per_core() > 1 {
        let mut views = crate::cpu_topo::views();
        for i in 0..MAX_CORES {
            if !want[i] {
                views[i].eligible = false;
            }
        }
        if let Some(cpu) = placement::pick_wake(&views) {
            wake_worker(cpu);
        }
    } else {
        for i in 1..MAX_CORES {
            if want[i] {
                wake_worker(i as u32);
            }
        }
    }
}

/// One evaluation pass. Called from the scheduler tick on the master core.
/// Scans online worker cores and issues IPI_HALT / IPI_WAKEUP.
#[no_mangle]
pub extern "C" fn energy_decision_tick() {
    // Only a multi-core configuration has workers to drive.
    if energy::energy_core_count_online() <= 1 {
        return;
    }

    let mut want = [false; MAX_CORES];

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

        // Core is halted/asleep: decide whether to wake it.  The actual IPI is
        // deferred to `wake_selected`, which is sibling-aware.
        let wake = energy_decision_should_wake(state, queue_len, 1, load, trend);
        if wake != 0 {
            if idle_ms >= IDLE_WAKE_MS {
                want[cpu as usize] = true;
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

    // Wake the chosen cores now that every candidate is known.
    wake_selected(&want);

    // Worker-initiated help: a saturated worker that still saw queued work asked
    // for a peer.  Consume the requests and wake one sleeping worker so the
    // queue drains on two cores (subject to the same idle/benefit rules and the
    // same sibling-aware preference).
    let mut help = false;
    for cpu in 1..MAX_CORES {
        if monitor::energy_monitor_take_help(cpu as u32) != 0 {
            help = true;
        }
    }
    if help && monitor::energy_monitor_queue_length(0) > 0 {
        let mut views = crate::cpu_topo::views();
        for i in 0..MAX_CORES {
            let cpu = i as u32;
            views[i].eligible = views[i].eligible
                && energy::energy_core_cstate(cpu) != CSTATE_C0
                && idle_ms_of(cpu) >= IDLE_WAKE_MS;
        }
        if let Some(cpu) = placement::pick_wake(&views) {
            wake_worker(cpu);
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
    // Offline a physical core only while the whole core is idle: never park a
    // logical CPU whose sibling is running work (the sibling must keep running).
    let views = crate::cpu_topo::views();
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
        if !placement::can_offline_core(&views, cpu) {
            continue;
        }
        // Park the whole physical core as one unit: every online worker thread
        // on this core is idle (the guard above), so take them all offline.
        let mask = placement::core_mask(&views, cpu);
        for s in 1..MAX_CORES {
            if mask & (1u64 << s) == 0 {
                continue;
            }
            let s = s as u32;
            if energy::energy_core_is_online(s) != 0 && energy::energy_core_role(s) == 2 {
                let _ = crate::smp::smp_cpu_offline(s);
            }
        }
        return;
    }
}
