//! Boot-time CPU topology (PLAN-2.0.0 P1.2).
//!
//! Reads CPUID leaf `0xB`/`0x1F` (falling back to leaf `1` HTT + leaf `4`) via
//! the C `cact_cpuid` primitive, decodes the levels with the pure, host-tested
//! `topology` module, and derives each logical CPU's package/core id from the
//! APIC ids the energy core map recorded.  Exposes the fields `/proc/cpuinfo`
//! prints (core id / physical id / siblings / cpu cores).
//!
//! This file is the kernel-side glue: it uses FFI and a global, so it is not
//! part of the host suite; the arithmetic lives in `topology`.

use core::cell::SyncUnsafeCell;

use crate::energy::{self, MAX_CORES};
use crate::ffi;
use crate::placement::{self, CoreView};
use crate::topology::{self, Level, Topology};

struct CpuTopo {
    valid: bool,
    threads_per_core: u32,
    cores_per_package: u32,
    logical_per_package: u32,
    packages: u32,
    package: [u32; MAX_CORES],
    core: [u32; MAX_CORES],
}

static TOPO: SyncUnsafeCell<CpuTopo> = SyncUnsafeCell::new(CpuTopo {
    valid: false,
    threads_per_core: 1,
    cores_per_package: 1,
    logical_per_package: 1,
    packages: 1,
    package: [0; MAX_CORES],
    core: [0; MAX_CORES],
});

fn cpuid(leaf: u32, subleaf: u32) -> (u32, u32, u32, u32) {
    let (mut a, mut b, mut c, mut d) = (0u32, 0u32, 0u32, 0u32);
    // SAFETY: `cact_cpuid` writes exactly the four out-pointers it is handed and
    // reads nothing else; the locals are live for the whole call.
    unsafe { ffi::cact_cpuid(leaf, subleaf, &mut a, &mut b, &mut c, &mut d) };
    (a, b, c, d)
}

/// Walk the subleaves of `leaf` until an invalid one, keeping the SMT and Core
/// level descriptors.
fn read_levels(leaf: u32) -> (Option<Level>, Option<Level>) {
    let mut smt: Option<Level> = None;
    let mut core: Option<Level> = None;
    for sub in 0..32u32 {
        let (eax, ebx, ecx, _edx) = cpuid(leaf, sub);
        let l = topology::decode_level(eax, ebx, ecx);
        if l.kind == topology::LEVEL_INVALID || l.count == 0 {
            break;
        }
        match l.kind {
            topology::LEVEL_SMT => smt = Some(l),
            topology::LEVEL_CORE => core = Some(l),
            _ => {}
        }
    }
    (smt, core)
}

fn probe() -> Option<Topology> {
    let (max_leaf, _b, _c, _d) = cpuid(0, 0);
    // Prefer the newer leaf 0x1F; fall back to 0xB.  A leaf can be enumerated
    // yet return no levels (some firmwares/emulators), so try both before the
    // HTT fallback.
    for leaf in [0x1f_u32, 0x0b_u32] {
        if max_leaf < leaf {
            continue;
        }
        let (smt, core) = read_levels(leaf);
        if smt.is_some() || core.is_some() {
            if let Some(t) = topology::topology_from_levels(smt, core) {
                return Some(t);
            }
        }
    }
    // HTT fallback: leaf 1 EBX[23:16] logical processors, leaf 4 cores/package.
    let (_a1, ebx1, _c1, _d1) = cpuid(1, 0);
    let logical = topology::logical_per_package_from_leaf1(ebx1);
    let (eax4, _b4, _c4, _d4) = cpuid(4, 0);
    let cores = topology::cores_per_package_from_leaf4(eax4);
    topology::topology_from_fallback(logical, cores)
}

/// Decode the topology and fill the per-logical-CPU table.  Run once at boot,
/// after `energy_init` (which records the APIC id of every present core).
#[no_mangle]
pub extern "C" fn cpu_topo_init() {
    let probed = probe();
    let topo = probed.unwrap_or(Topology {
        smt_shift: 0,
        core_shift: 0,
        threads_per_core: 1,
        cores_per_package: 1,
        logical_per_package: 1,
    });

    // SAFETY: `cpu_topo_init` runs once during single-threaded boot, before any
    // reader of `TOPO` can run.
    let st = unsafe { &mut *TOPO.get() };
    st.valid = probed.is_some();
    st.threads_per_core = topo.threads_per_core;
    st.cores_per_package = topo.cores_per_package;
    st.logical_per_package = topo.logical_per_package;

    let mut max_pkg = 0u32;
    for cpu in 0..MAX_CORES as u32 {
        let apic = energy::energy_core_lapic_id(cpu);
        let pkg = topology::package_id(&topo, apic);
        let core = topology::core_id(&topo, apic);
        st.package[cpu as usize] = pkg;
        st.core[cpu as usize] = core;
        if energy::energy_core_is_present(cpu) != 0 && pkg + 1 > max_pkg {
            max_pkg = pkg + 1;
        }
    }
    st.packages = if max_pkg == 0 { 1 } else { max_pkg };
}

fn with<R>(f: impl FnOnce(&CpuTopo) -> R) -> R {
    // SAFETY: `TOPO` is written once by `cpu_topo_init` at boot and only read
    // afterwards, so a shared borrow cannot race a writer.
    let st = unsafe { &*TOPO.get() };
    f(st)
}

/// Package (socket) id of logical CPU `cpu`.
pub fn package(cpu: u32) -> u32 {
    with(|st| if (cpu as usize) < MAX_CORES { st.package[cpu as usize] } else { 0 })
}

/// Core id within the package of logical CPU `cpu`.
pub fn core(cpu: u32) -> u32 {
    with(|st| if (cpu as usize) < MAX_CORES { st.core[cpu as usize] } else { 0 })
}

/// Logical processors sharing one physical core.
pub fn threads_per_core() -> u32 {
    with(|st| st.threads_per_core)
}

/// Number of distinct physical packages seen.
pub fn packages() -> u32 {
    with(|st| st.packages)
}

/// Snapshot every logical CPU as a [`CoreView`] for the placement / idle-depth
/// policy (`crate::placement`).
pub fn views() -> [CoreView; MAX_CORES] {
    let mut v = [CoreView::empty(0); MAX_CORES];
    for i in 0..MAX_CORES {
        let cpu = i as u32;
        let online = energy::energy_core_is_present(cpu) != 0
            && energy::energy_core_is_online(cpu) != 0;
        v[i] = CoreView {
            cpu,
            package: package(cpu),
            core: core(cpu),
            online,
            eligible: online && energy::energy_core_role(cpu) == 2,
            awake: energy::energy_core_cstate(cpu) == crate::energy_model::CSTATE_C0,
            idle: energy::energy_core_is_idle(cpu) != 0,
        };
    }
    v
}

#[no_mangle]
pub extern "C" fn cpu_topo_valid() -> i32 {
    with(|st| st.valid as i32)
}

#[no_mangle]
pub extern "C" fn cpu_topo_package(cpu: u32) -> u32 {
    package(cpu)
}

#[no_mangle]
pub extern "C" fn cpu_topo_core(cpu: u32) -> u32 {
    core(cpu)
}

#[no_mangle]
pub extern "C" fn cpu_topo_siblings(_cpu: u32) -> u32 {
    with(|st| st.logical_per_package)
}

#[no_mangle]
pub extern "C" fn cpu_topo_cpu_cores(_cpu: u32) -> u32 {
    with(|st| st.cores_per_package)
}

#[no_mangle]
pub extern "C" fn cpu_topo_threads_per_core() -> u32 {
    threads_per_core()
}

#[no_mangle]
pub extern "C" fn cpu_topo_packages() -> u32 {
    packages()
}

/// Deterministic check of the sibling-aware placement policy against the *real*
/// machine topology: with the master busy and every worker idle + sleeping, the
/// pick must land on a different physical core than the master when SMT is
/// present and there is more than one physical core.  Returns 0 on success.
#[no_mangle]
pub extern "C" fn placement_selftest() -> i32 {
    let tpc = threads_per_core();
    let present = energy::energy_core_count_present();
    if present < 2 {
        return 0; // single logical CPU: nothing to place
    }

    // Synthetic snapshot: master busy, every other logical CPU eligible + idle.
    let mut views = [CoreView::empty(0); MAX_CORES];
    for i in 0..MAX_CORES {
        let cpu = i as u32;
        views[i] = CoreView {
            cpu,
            package: package(cpu),
            core: core(cpu),
            online: cpu < present,
            eligible: cpu != 0 && cpu < present,
            awake: false,
            idle: cpu != 0,
        };
    }
    views[0].online = true;
    views[0].eligible = false;
    views[0].idle = false; // master busy

    let Some(chosen) = placement::pick_wake(&views) else {
        return 0;
    };
    if tpc > 1 && present >= 2 * tpc {
        // Two SMT threads exist and at least two physical cores: the second
        // thread must not share the master's core.
        if placement::same_core(&views[chosen as usize], &views[0]) {
            return 1;
        }
    }
    0
}
