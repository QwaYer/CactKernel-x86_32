//! Energy governor self-tests (Step 7 of the plan).
//!
//! Unit-style checks for the C-state transition rules, the benefit/cost model,
//! the sleep/wake policy thresholds, the MLFQ<->C-state mapping and the budget
//! accounting.  Runs once at boot from the C kernel (`energy_selftest`) and
//! returns the number of failed checks (0 = all passed).  Integration,
//! benchmark and latency tests run under QEMU with the userland harness.

use crate::cstate;
use crate::decision;
use crate::energy;
use crate::mlfq_map;
use crate::monitor;

use cstate::{CSTATE_C0, CSTATE_C1, CSTATE_C3, CSTATE_C6};
use monitor::{TREND_DOWN, TREND_STABLE, TREND_UP};

fn check(fails: &mut u32, ok: bool) {
    if !ok {
        *fails += 1;
    }
}

#[no_mangle]
pub extern "C" fn energy_selftest() -> i32 {
    let mut fails = 0u32;

    // --- C-state transition rules -------------------------------------------------
    check(&mut fails, energy::energy_core_is_master(0) == 1);
    check(
        &mut fails,
        energy::energy_core_set_cstate(0, CSTATE_C1) == 0, // master: C1 ok
    );
    check(
        &mut fails,
        energy::energy_core_set_cstate(0, CSTATE_C3) != 0, // master: deep forbidden
    );
    check(
        &mut fails,
        energy::energy_core_set_cstate(0, CSTATE_C0) == 0, // restore active
    );

    // --- Benefit / cost model ------------------------------------------------------
    let c1 = decision::energy_decision_cost(CSTATE_C1);
    let c3 = decision::energy_decision_cost(CSTATE_C3);
    let c6 = decision::energy_decision_cost(CSTATE_C6);
    check(&mut fails, c1 > 0 && c1 <= c3 && c3 <= c6);

    let b0 = decision::energy_decision_benefit(0, 1);
    let b10 = decision::energy_decision_benefit(10, 1);
    let b1000 = decision::energy_decision_benefit(1000, 1);
    check(&mut fails, b0 == 0 && b10 > b0 && b1000 > b10);
    check(
        &mut fails,
        b1000 * 2 > c6 * 3, // heavy queue must beat cost*1.5
    );

    // --- Policy thresholds ---------------------------------------------------------
    // C0 core is never "woken".
    check(
        &mut fails,
        decision::energy_decision_should_wake(CSTATE_C0, 100, 1, 900, TREND_UP) == 0,
    );
    // Deep core + huge queue wakes on the formula alone (low load, down trend).
    check(
        &mut fails,
        decision::energy_decision_should_wake(CSTATE_C6, 1000, 1, 0, TREND_DOWN) == 1,
    );
    // Overload rule and proactive (rising load) rule.
    check(
        &mut fails,
        decision::energy_decision_should_wake(CSTATE_C1, 0, 1, 900, TREND_STABLE) == 1,
    );
    check(
        &mut fails,
        decision::energy_decision_should_wake(CSTATE_C1, 0, 1, 900, TREND_UP) == 1,
    );
    // Light load never wakes.
    check(
        &mut fails,
        decision::energy_decision_should_wake(CSTATE_C6, 1, 1, 100, TREND_STABLE) == 0,
    );

    check(&mut fails, decision::energy_decision_should_sleep(100, 150) == 1);
    check(&mut fails, decision::energy_decision_should_sleep(100, 50) == 0);
    check(&mut fails, decision::energy_decision_should_sleep(900, 150) == 0);

    // --- MLFQ <-> C-state mapping --------------------------------------------------
    check(&mut fails, mlfq_map::energy_mlfq_cap_for_level(0) == CSTATE_C0);
    check(&mut fails, mlfq_map::energy_mlfq_cap_for_level(1) == CSTATE_C1);
    check(&mut fails, mlfq_map::energy_mlfq_cap_for_level(2) == CSTATE_C3);
    check(&mut fails, mlfq_map::energy_mlfq_cap_for_level(3) == CSTATE_C6);
    // The master may idle at C1 at most, whatever the runqueue says.
    check(&mut fails, mlfq_map::energy_mlfq_idle_target(0) == CSTATE_C1);

    // --- Budget accounting ---------------------------------------------------------
    monitor::energy_monitor_refill_budget(0);
    let before = monitor::energy_monitor_energy_budget(0);
    monitor::energy_monitor_charge_energy(0, 100);
    let after = monitor::energy_monitor_energy_budget(0);
    check(&mut fails, before == 10000 && after == before.saturating_sub(100));
    monitor::energy_monitor_refill_budget(0);

    fails as i32
}
