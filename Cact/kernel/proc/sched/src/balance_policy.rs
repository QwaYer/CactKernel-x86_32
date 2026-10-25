//! Pure load-balance policy: which core should hand one ready task to which.
//!
//! Step 6 of the energy governor splits in two halves.  This module is the
//! *decision* half: given a per-core snapshot (onlineness, smoothed load, number
//! of tasks waiting on that core's own ready queue) it names the `(src, dst)`
//! pair a migration should go between.  `balance.rs` is the *action* half: it
//! builds the snapshot and performs the move on the runqueues.
//!
//! Keeping the decision pure (no hardware, no globals, no FFI) means it is
//! unit-tested on the host by `Kernel-Unit-Tests-for-Cact/`, alongside the
//! sibling-aware placement policy it complements.

/// One logical CPU as seen by the load balancer.
#[derive(Copy, Clone, PartialEq, Eq, Debug)]
pub struct CoreLoad {
    /// Logical CPU index (also the tie-break order).
    pub cpu: u32,
    /// present && online.
    pub online: bool,
    /// Smoothed load (per-mille, 1000 = 100%) sampled by the monitor.
    pub load_permille: u32,
    /// Runnable tasks waiting on this core's *own* ready queue.
    pub runnable: u32,
}

impl CoreLoad {
    /// Default view for a CPU index (offline, empty).
    pub const fn empty(cpu: u32) -> Self {
        Self {
            cpu,
            online: false,
            load_permille: 0,
            runnable: 0,
        }
    }
}

/// Choose the `(src, dst)` pair for one task migration, or `None` when the
/// machine is balanced enough.
///
/// `src` is the busiest online core that actually holds runnable work — a core
/// loaded only by the task it is currently running has nothing it can give
/// away — and `dst` is the least-loaded online core.  A migration is proposed
/// only when the load spread reaches `threshold_permille`.  Ties break to the
/// lowest CPU index, so the choice is deterministic.
pub fn pick_migration(cores: &[CoreLoad], threshold_permille: u32) -> Option<(u32, u32)> {
    let mut src: Option<&CoreLoad> = None;
    for c in cores {
        if !c.online || c.runnable == 0 {
            continue;
        }
        if src.is_none_or(|s| c.load_permille > s.load_permille) {
            src = Some(c);
        }
    }
    let src = src?;

    let mut dst: Option<&CoreLoad> = None;
    for c in cores {
        if !c.online || c.cpu == src.cpu {
            continue;
        }
        if dst.is_none_or(|d| c.load_permille < d.load_permille) {
            dst = Some(c);
        }
    }
    let dst = dst?;

    if src.load_permille.saturating_sub(dst.load_permille) >= threshold_permille {
        Some((src.cpu, dst.cpu))
    } else {
        None
    }
}
