//! Pure CPUID topology decoding (Intel leaf `0xB`/`0x1F`, with the older
//! leaf `4` + leaf `1` HTT fallback).
//!
//! The kernel feeds raw CPUID register values in; this module contains no asm,
//! no globals and no FFI, so it is the single source of truth for the topology
//! arithmetic and is unit-tested on the host by `Kernel-Unit-Tests-for-Cact/`
//! (P2.1).  Wiring the raw CPUID reads to it is part of P1.2 (SMT topology).

/// CPUID `ECX[7:0]` level-type values (leaf 0xB/0x1F).
pub const LEVEL_INVALID: u32 = 0;
pub const LEVEL_SMT: u32 = 1;
pub const LEVEL_CORE: u32 = 2;

/// One level descriptor from leaf `0xB`/`0x1F`.
#[derive(Copy, Clone, PartialEq, Eq, Debug)]
pub struct Level {
    /// `EAX[4:0]`: bits to shift the x2APIC id right to get this level's id.
    pub shift: u32,
    /// `EBX[15:0]`: logical processors at and below this level.
    pub count: u32,
    /// `ECX[15:8]`: level type (`LEVEL_SMT` / `LEVEL_CORE` / `LEVEL_INVALID`).
    pub kind: u32,
}

/// Decode one leaf-`0xB`/`0x1F` subleaf from its raw registers.  Per the SDM
/// (and Linux `topology_ext.c`), `ECX[7:0]` is the level *number* and
/// `ECX[15:8]` is the level *type* (1 = SMT, 2 = Core, 0 = invalid).
pub const fn decode_level(eax: u32, ebx: u32, ecx: u32) -> Level {
    Level {
        shift: eax & 0x1f,
        count: ebx & 0xffff,
        kind: (ecx >> 8) & 0xff,
    }
}

/// Resolved topology for one package.
#[derive(Copy, Clone, PartialEq, Eq, Debug)]
pub struct Topology {
    pub smt_shift: u32,
    pub core_shift: u32,
    pub threads_per_core: u32,
    pub cores_per_package: u32,
    pub logical_per_package: u32,
}

/// Combine the SMT and Core level descriptors into a topology.  Either may be
/// absent (single-threaded / single-core), in which case sensible defaults are
/// used.  Returns `None` only if the inputs are self-contradictory
/// (`logical < threads`).
pub fn topology_from_levels(smt: Option<Level>, core: Option<Level>) -> Option<Topology> {
    let (smt_shift, threads) = match smt {
        Some(l) if l.kind == LEVEL_SMT && l.count > 0 => (l.shift, l.count),
        _ => (0, 1),
    };
    let (core_shift, logical) = match core {
        Some(l) if l.kind == LEVEL_CORE && l.count > 0 => (l.shift, l.count),
        _ => (smt_shift, threads),
    };
    if threads == 0 || logical < threads {
        return None;
    }
    let cores = (logical / threads).max(1);
    Some(Topology {
        smt_shift,
        core_shift,
        threads_per_core: threads,
        cores_per_package: cores,
        logical_per_package: logical,
    })
}

/// Package (socket) id for an x2APIC id.
pub const fn package_id(t: &Topology, x2apic: u32) -> u32 {
    x2apic >> t.core_shift
}

/// Core id within the package for an x2APIC id.
pub fn core_id(t: &Topology, x2apic: u32) -> u32 {
    if t.cores_per_package == 0 {
        return 0;
    }
    (x2apic >> t.smt_shift) % t.cores_per_package
}

/// Logical processors per package from leaf `1` `EBX[23:16]` (the HTT fallback).
pub const fn logical_per_package_from_leaf1(ebx: u32) -> u32 {
    (ebx >> 16) & 0xff
}

/// Cores per package from leaf `4` `EAX[31:26]` + 1 (the HTT fallback).
pub const fn cores_per_package_from_leaf4(eax: u32) -> u32 {
    ((eax >> 26) & 0x3f) + 1
}

/// Topology from the HTT fallback (leaf 1 + leaf 4), used when leaf `0xB`/`0x1F`
/// is unavailable.
pub fn topology_from_fallback(logical: u32, cores: u32) -> Option<Topology> {
    if logical == 0 || cores == 0 || logical < cores {
        return None;
    }
    let threads = logical / cores;
    if threads == 0 {
        return None;
    }
    // `smt_shift` = log2(threads); `core_shift` is the package shift = the bits
    // covering a whole package = log2(logical per package), matching the Core
    // level's EAX in leaf 0xB (NOT log2(cores)).
    let smt_shift = if threads.is_power_of_two() { threads.trailing_zeros() } else { 0 };
    let core_shift = if logical.is_power_of_two() { logical.trailing_zeros() } else { 0 };
    Some(Topology {
        smt_shift,
        core_shift,
        threads_per_core: threads,
        cores_per_package: cores,
        logical_per_package: logical,
    })
}
