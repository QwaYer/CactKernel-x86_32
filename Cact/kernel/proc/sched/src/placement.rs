//! Pure sibling-aware core placement policy (PLAN-2.0.0 P1.2).
//!
//! On an SMT machine two busy threads must not both sit on the two logical CPUs
//! of one physical core while other physical cores sit idle.  CactOS uses one
//! shared MLFQ (no per-core runqueues), so placement is expressed where a core
//! is *chosen*: the energy governor picks which sleeping worker to wake, and
//! this module decides which one, preferring a physical core with no busy
//! sibling ("fresh") over one whose sibling is already busy ("shared").
//!
//! No hardware access, no globals and no FFI, so it is unit-tested on the host
//! by `Kernel-Unit-Tests-for-Cact/` (P2.1).

use crate::energy_model::CSTATE_C1;

/// One logical CPU as seen by the placement policy.
#[derive(Copy, Clone, PartialEq, Eq, Debug)]
pub struct CoreView {
    /// Logical CPU index (also the tie-break order).
    pub cpu: u32,
    /// Physical package (socket) id.
    pub package: u32,
    /// Core id within the package.
    pub core: u32,
    /// present && online.
    pub online: bool,
    /// A candidate for this decision (online worker passing the caller's gate).
    pub eligible: bool,
    /// Awake (C0).
    pub awake: bool,
    /// Running its idle task (so its physical core is free).
    pub idle: bool,
}

impl CoreView {
    /// Default view for a CPU index (offline, idle, not a candidate).
    pub const fn empty(cpu: u32) -> Self {
        Self {
            cpu,
            package: 0,
            core: 0,
            online: false,
            eligible: false,
            awake: false,
            idle: true,
        }
    }
}

/// Same physical core (package+core)?
pub const fn same_core(a: &CoreView, b: &CoreView) -> bool {
    a.package == b.package && a.core == b.core
}

/// A logical CPU that is currently occupying its physical core: online and not
/// running its idle task.
pub const fn busy(v: &CoreView) -> bool {
    v.online && !v.idle
}

/// True when another logical CPU on `cpu`'s physical core is busy — waking
/// `cpu` would make two busy threads share one physical core.
pub fn sibling_busy(views: &[CoreView], cpu: u32) -> bool {
    let Some(me) = views.iter().find(|v| v.cpu == cpu) else {
        return false;
    };
    views
        .iter()
        .any(|v| v.cpu != me.cpu && same_core(v, me) && busy(v))
}

/// Choose which eligible sleeping worker to wake, preferring a fresh physical
/// core over one whose sibling is already busy (ties broken by lowest cpu).
/// Returns the chosen cpu, or `None` when there is nothing to wake.
pub fn pick_wake(views: &[CoreView]) -> Option<u32> {
    let mut fresh: Option<u32> = None;
    let mut shared: Option<u32> = None;
    for v in views {
        if !v.eligible || v.awake {
            continue;
        }
        if sibling_busy(views, v.cpu) {
            if shared.is_none_or(|c| v.cpu < c) {
                shared = Some(v.cpu);
            }
        } else if fresh.is_none_or(|c| v.cpu < c) {
            fresh = Some(v.cpu);
        }
    }
    fresh.or(shared)
}

/// Cap a requested idle depth for `cpu`: a logical CPU must not idle deeper
/// than C1 while a sibling on its physical core is busy, because C3/C6 are
/// physical-core states that need the whole core idle.  C1 (or shallower) is
/// passed through unchanged.
pub fn cap_idle_depth(requested: u32, views: &[CoreView], cpu: u32) -> u32 {
    if requested > CSTATE_C1 && sibling_busy(views, cpu) {
        CSTATE_C1
    } else {
        requested
    }
}

/// A physical core may be offlined/parked only while the whole core is idle:
/// never park a logical CPU whose sibling is running work (the sibling must
/// keep running).
pub fn can_offline_core(views: &[CoreView], cpu: u32) -> bool {
    !sibling_busy(views, cpu)
}

/// Bitmask of logical CPU indices sharing `cpu`'s physical core (including
/// `cpu` itself).  A physical core is parked as one unit by iterating this mask.
/// Indices are < 64 (MAX_CORES).
pub fn core_mask(views: &[CoreView], cpu: u32) -> u64 {
    let Some(me) = views.iter().find(|v| v.cpu == cpu) else {
        return 0;
    };
    let mut m = 0u64;
    for v in views {
        if same_core(v, me) {
            m |= 1u64 << (v.cpu & 63);
        }
    }
    m
}
