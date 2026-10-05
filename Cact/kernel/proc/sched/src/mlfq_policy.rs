//! Pure MLFQ scheduling policy: level ids, quanta, level clamping, the
//! highest-runnable-level selection, and the periodic priority boost.
//!
//! No hardware access, no globals and no FFI, so this is the single source of
//! truth for the MLFQ policy and is unit-tested on the host by
//! `Kernel-Unit-Tests-for-Cact/` (P2.1).  The intrusive queue mechanics live in
//! `intrusive_queue.rs`; `mlfq.rs` wires both into the scheduler.

/// Number of feedback levels.
pub const MLFQ_LEVELS: usize = 4;

pub const MLFQ_LEVEL_RT: u32 = 0;
pub const MLFQ_LEVEL_INTERACTIVE: u32 = 1;
pub const MLFQ_LEVEL_NORMAL: u32 = 2;
pub const MLFQ_LEVEL_BACKGROUND: u32 = 3;

/// Tick quantum per level: RT is preempted rarely, interactive often.
pub const MLFQ_QUANTUM: [u32; MLFQ_LEVELS] = [5, 1, 2, 4];

/// Period (in ticks) of the anti-starvation priority boost.
pub const BOOST_INTERVAL: u32 = 50;
/// Level that boosted tasks are promoted to.
pub const BOOST_TARGET: u32 = MLFQ_LEVEL_INTERACTIVE;

/// Clamp a level/priority into `[0, MLFQ_LEVELS)`.
pub const fn clamp_level(level: u32) -> u32 {
    if level >= MLFQ_LEVELS as u32 {
        MLFQ_LEVELS as u32 - 1
    } else {
        level
    }
}

/// Quantum (in ticks) for a level, clamped like [`clamp_level`].
pub const fn quantum_for(level: u32) -> u32 {
    MLFQ_QUANTUM[clamp_level(level) as usize]
}

/// Highest-priority (lowest-index) non-empty level, or `None` when idle.
pub fn pick_highest_level(counts: &[u32]) -> Option<usize> {
    counts.iter().position(|&c| c > 0)
}

/// A task at `priority` should drop a level after its quantum.  RT never drops;
/// background is the floor.  Returns the next priority.
pub const fn demote_on_quantum(priority: u32) -> u32 {
    if priority != MLFQ_LEVEL_RT && priority < MLFQ_LEVEL_BACKGROUND {
        priority + 1
    } else {
        priority
    }
}

/// Whether the boost counter (incrementing once per tick) triggers a boost.
pub const fn boost_due(counter: u32) -> bool {
    counter >= BOOST_INTERVAL
}
