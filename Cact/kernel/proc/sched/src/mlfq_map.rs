//! MLFQ <-> C-state mapping (Step 5 of the energy governor).
//!
//! Maps the four MLFQ levels onto core idle depths.  The interactive classes
//! need fast wakeups, so a core that may have to run them must not sleep
//! deeper than the class's cap:
//!
//! ```text
//! MLFQ level          role in Cact       max core idle depth
//!   0  (RT)           real-time          C0 (never sleeps)
//!   1  (interactive)  interactive        C1
//!   2  (normal)       normal             C3
//!   3  (background)   background         C6
//! ```
//!
//! The highest-priority runnable queue on a core therefore bounds how deep
//! that core may idle, and enqueueing into a queue that needs a shallower
//! core wakes a too-deep worker (after a benefit/cost check).

use crate::energy::{self, MAX_CORES};
use crate::mlfq;
use crate::monitor;
use crate::cstate;
use crate::decision;

use cstate::{CSTATE_C0, CSTATE_C1, CSTATE_C3, CSTATE_C6};

/// Deepest idle state acceptable while a task of `level` may run next.
fn idle_cap_for_level(level: u32) -> u32 {
    match level {
        0 => CSTATE_C0, // RT: core must stay fully active
        1 => CSTATE_C1, // interactive
        2 => CSTATE_C3, // normal
        _ => CSTATE_C6, // background
    }
}

/// Cap on this core's idle depth derived from the global runqueue.
pub(crate) fn runqueue_idle_cap() -> u32 {
    match mlfq::mlfq_highest_runnable_level() {
        Some(level) => idle_cap_for_level(level),
        None => CSTATE_C6,
    }
}

#[no_mangle]
pub extern "C" fn energy_mlfq_cap_for_level(level: u32) -> u32 {
    idle_cap_for_level(level)
}

#[no_mangle]
pub extern "C" fn energy_mlfq_runqueue_cap() -> u32 {
    runqueue_idle_cap()
}

/// C-state the energy governor should use when `cpu` idles next.
#[no_mangle]
pub extern "C" fn energy_mlfq_idle_target(cpu: u32) -> u32 {
    let role = energy::energy_core_role(cpu);
    let role_max = if role == 1 {
        CSTATE_C1 // master: never deeper than C1
    } else if cstate::energy_cstate_available(CSTATE_C6) != 0 {
        CSTATE_C6
    } else if cstate::energy_cstate_available(CSTATE_C3) != 0 {
        CSTATE_C3
    } else {
        CSTATE_C1
    };

    let cap = runqueue_idle_cap();
    let target = if role_max < cap { role_max } else { cap };
    // An idle core must always halt with at least C1 semantics; C0 means the
    // core should not be in the idle path at all.
    if target < CSTATE_C1 {
        CSTATE_C1
    } else {
        target
    }
}

/// Called whenever a task lands in `level`: if a worker sleeping too deep for
/// that class exists, wake it — but only when the benefit/cost model says the
/// pending work is worth the transition energy.
pub(crate) fn on_enqueue(level: u32) {
    // UP build: no online workers yet, nothing to wake.
    if energy::energy_core_count_online() <= 1 {
        return;
    }
    let required = idle_cap_for_level(level);

    for cpu in 1..MAX_CORES {
        let cpu = cpu as u32;
        if energy::energy_core_is_present(cpu) == 0 || energy::energy_core_is_online(cpu) == 0 {
            continue;
        }
        if energy::energy_core_role(cpu) != 2 {
            continue;
        }

        let state = energy::energy_core_cstate(cpu);
        if state == CSTATE_C0 || state <= required {
            continue; // awake or shallow enough for this class
        }

        // The core is deeper than this queue class tolerates: run the
        // benefit/cost gate before issuing the wakeup IPI.
        let queue_len = mlfq::mlfq_runnable_count();
        let load = monitor::energy_monitor_load_avg(cpu);
        let trend = monitor::energy_monitor_trend(cpu);
        if decision::energy_decision_should_wake(state, queue_len, level + 1, load, trend) == 0 {
            continue;
        }
        if cstate::energy_ipi_wake_worker(cpu) == 0 {
            let _ = energy::energy_core_set_cstate(cpu, CSTATE_C0);
            let _ = monitor::energy_monitor_charge_energy(
                cpu,
                decision::energy_decision_cost(state) as u32,
            );
        }
    }
}
