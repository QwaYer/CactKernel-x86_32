//! C-state controller (Step 2 of the energy governor).
//!
//! Owns the per-platform C-state descriptors (calibrated residency/latency and
//! energy costs), validates and performs role-aware C-state transitions, and
//! implements the master-driven IPI_HALT / IPI_WAKEUP protocol for worker
//! cores.
//!
//! State availability model on this port:
//!   * C1 is always executable (HLT).
//!   * C3/C6 descriptors are reserved and currently reported unavailable:
//!     entering them for real requires an ACPI `_CST`/MWAIT driver, which is
//!     not present yet.  The governor (Step 4) reads `energy_cstate_available`
//!     before selecting a target state, so deep states are never requested
//!     until the platform supports them.
//!
//! Roles: the master core may use C0/C1 only (it must stay quickly wakeable
//! to orchestrate workers); worker cores may use any supported state.

use core::cell::SyncUnsafeCell;
use core::sync::atomic::AtomicBool;

use crate::ffi;
use crate::energy::{self, MAX_CORES};

// Numeric C-state ids must match Cact/kernel/energy/energy.h; the pure model
// (`crate::energy_model`) owns the values so the kernel and the host tests can
// never disagree.
pub const CSTATE_C0: u32 = crate::energy_model::CSTATE_C0;
pub const CSTATE_C1: u32 = crate::energy_model::CSTATE_C1;
pub const CSTATE_C3: u32 = crate::energy_model::CSTATE_C3;
pub const CSTATE_C6: u32 = crate::energy_model::CSTATE_C6;
const CSTATE_COUNT: usize = 4;

// LAPIC IPI vectors. 0xF8/0xF9 were stray-guard gates in the IDT; the C-state
// controller re-arms them for its own protocol.
pub const IPI_HALT_VECTOR: u32 = 0xF8;
pub const IPI_WAKE_VECTOR: u32 = 0xF9;

#[derive(Copy, Clone)]
struct CStateInfo {
    available: bool,
    latency_us: u32,
    min_residency_us: u32,
    wakeup_energy: u32,
    cache_harm_energy: u32,
    /// MWAIT hint (EAX) that enters this state, or -1 when it has no MWAIT
    /// encoding (C0) or is not yet known (deep states await the ACPI `_CST`
    /// probe).  When non-negative and the CPU has MONITOR/MWAIT, the idle path
    /// uses `mwait` instead of `hlt`.
    mwait_hint: i32,
}

const fn cstate_info_default(
    available: bool,
    latency_us: u32,
    min_residency_us: u32,
    wakeup_energy: u32,
    cache_harm_energy: u32,
    mwait_hint: i32,
) -> CStateInfo {
    CStateInfo {
        available,
        latency_us,
        min_residency_us,
        wakeup_energy,
        cache_harm_energy,
        mwait_hint,
    }
}

// Published per-state characteristics for a VM (relative energy units). Real
// values would come from ACPI _CST latency + RAPL/MSR calibration; the wakeup
// and cache-harm costs come from the pure `energy_model` so the benefit/cost
// decision (also in `energy_model`) and this table are one source of truth.
static CSTATES: SyncUnsafeCell<[CStateInfo; CSTATE_COUNT]> = SyncUnsafeCell::new([
    cstate_info_default(true, 0, 0,
        crate::energy_model::wakeup_energy(CSTATE_C0),
        crate::energy_model::cache_harm_energy(CSTATE_C0), -1),            // C0
    cstate_info_default(true, 2, 40,
        crate::energy_model::wakeup_energy(CSTATE_C1),
        crate::energy_model::cache_harm_energy(CSTATE_C1), 0x00),          // C1 (MWAIT hint 0x00)
    cstate_info_default(false, 60, 300,
        crate::energy_model::wakeup_energy(CSTATE_C3),
        crate::energy_model::cache_harm_energy(CSTATE_C3), -1),            // C3 (hint from _CST)
    cstate_info_default(false, 200, 2000,
        crate::energy_model::wakeup_energy(CSTATE_C6),
        crate::energy_model::cache_harm_energy(CSTATE_C6), -1),            // C6 (hint from _CST)
]);

static CSTATE_INIT_DONE: SyncUnsafeCell<bool> = SyncUnsafeCell::new(false);
static IPI_INIT_DONE: AtomicBool = AtomicBool::new(false);

unsafe extern "C" {
    fn ipi_halt_isr();
    fn ipi_wake_isr();
}

fn cstates() -> &'static mut [CStateInfo; CSTATE_COUNT] {
    // SAFETY: `CSTATES` is a statically initialised descriptor table that is only ever read at
    // runtime (`energy_cstate_*` all read one entry), so no aliasing write can occur while the
    // returned reference is live.
    unsafe { &mut *CSTATES.get() }
}

fn init_done() -> &'static mut bool {
    // SAFETY: `CSTATE_INIT_DONE` is set exactly once by `energy_cstate_init`, which runs from
    // single-threaded boot bring-up before the governor can be queried.
    unsafe { &mut *CSTATE_INIT_DONE.get() }
}

fn cstate_in_range(state: u32) -> bool {
    (state as usize) < CSTATE_COUNT
}

fn hlt() {
    // SAFETY: `hlt` only halts this CPU until the next interrupt; it reads and writes no memory
    // and leaves no flags to observe.
    unsafe {
        core::arch::asm!("hlt", options(nomem, nostack));
    }
}

/// Enter a hardware C-state with the MONITOR/MWAIT hint `hint`.  Only valid
/// when CPUID advertises MONITOR/MWAIT (the caller checks); otherwise the
/// instruction is undefined.
fn mwait_idle(hint: u32) {
    // MONITOR a private stack word (never written) so MWAIT exits only on an
    // interrupt, then `sti; mwait` with ECX bit 0 = interrupt-break: interrupts
    // must be enabled at the exact moment MWAIT executes or a queued timer
    // would be missed and the core would sleep past its next tick.
    let line: u32 = 0;
    // SAFETY: `monitor`/`mwait` only affect this CPU's idle state.  The
    // monitored address is a live stack local that nothing writes, so MWAIT
    // wakes on an interrupt (ECX bit 0 requests interrupt-break).
    unsafe {
        core::arch::asm!(
            "monitor",
            in("eax") (&line as *const u32) as u32,
            in("ecx") 0u32,
            in("edx") 0u32,
            options(nostack)
        );
        core::arch::asm!(
            "sti; mwait",
            in("eax") hint,
            in("ecx") 1u32,     // MWAIT_ECX_INTERRUPT_BREAK
            options(nostack)
        );
    }
}

/// Publish a C-state's real characteristics, from the ACPI `_CST` probe: its
/// MWAIT hint, wake latency, and whether the platform offers it.  Until this is
/// called for a deep state it stays unavailable and the governor never picks it.
///
/// # Safety
///
/// Must be called during single-threaded boot bring-up (the `_CST` probe), before
/// any worker core runs.
#[no_mangle]
pub unsafe extern "C" fn energy_cstate_configure(
    state: u32,
    available: i32,
    latency_us: u32,
    mwait_hint: i32,
) -> i32 {
    if !cstate_in_range(state) {
        return -1;
    }
    let c = &mut cstates()[state as usize];
    c.available = available != 0;
    if latency_us != 0 {
        c.latency_us = latency_us;
    }
    c.mwait_hint = mwait_hint;
    0
}

// ---------------------------------------------------------------------------
// C ABI
// ---------------------------------------------------------------------------

#[no_mangle]
pub extern "C" fn energy_cstate_init() -> i32 {
    if *init_done() {
        return 0;
    }
    // Read the platform's C-state descriptors (`_CST`) and publish their real
    // latencies and MWAIT hints.  If the firmware exposes none, the deep states
    // stay unavailable and the governor keeps using C1.
    // SAFETY: single-threaded boot bring-up, after ACPI is initialised;
    // `acpi_cstates_probe` only evaluates ACPI methods and calls back into
    // `energy_cstate_configure`.
    unsafe { ffi::acpi_cstates_probe() };
    *init_done() = true;
    0
}

#[no_mangle]
pub extern "C" fn energy_cstate_available(state: u32) -> i32 {
    if !cstate_in_range(state) {
        return 0;
    }
    cstates()[state as usize].available as i32
}

#[no_mangle]
pub extern "C" fn energy_cstate_latency_us(state: u32) -> u32 {
    if !cstate_in_range(state) {
        return 0;
    }
    cstates()[state as usize].latency_us
}

#[no_mangle]
pub extern "C" fn energy_cstate_min_residency_us(state: u32) -> u32 {
    if !cstate_in_range(state) {
        return 0;
    }
    cstates()[state as usize].min_residency_us
}

#[no_mangle]
pub extern "C" fn energy_cstate_wakeup_energy(state: u32) -> u32 {
    if !cstate_in_range(state) {
        return 0;
    }
    cstates()[state as usize].wakeup_energy
}

#[no_mangle]
pub extern "C" fn energy_cstate_cache_harm_energy(state: u32) -> u32 {
    if !cstate_in_range(state) {
        return 0;
    }
    cstates()[state as usize].cache_harm_energy
}

/// Validate and perform a role-aware transition of `cpu` to `state`.
/// This updates the recorded C-state; it does not put the CPU to sleep.
#[no_mangle]
pub extern "C" fn energy_cstate_enter(cpu: u32, state: u32) -> i32 {
    if !cstate_in_range(state) || !cstates()[state as usize].available {
        return -1;
    }
    energy::energy_core_set_cstate(cpu, state)
}

/// Block in the CPU idle state chosen for `cpu` until an interrupt arrives.
/// Safe to call repeatedly from an idle loop; always ends up halted for C1,
/// and (once deep states become available) enters the selected state.
#[no_mangle]
pub extern "C" fn energy_cstate_idle(cpu: u32) -> i32 {
    if energy::energy_core_is_online(cpu) == 0 {
        return -1;
    }

    // The MLFQ<->C-state mapping picks the target depth from the core role
    // and the runqueue contents (Step 5).
    let target = crate::mlfq_map::energy_mlfq_idle_target(cpu);
    let _ = energy::energy_core_set_cstate(cpu, target);

    // Cooperate with a physical-offline request.  Park HERE, where this core
    // holds no global lock (the energy/mlfq calls above have all returned): the
    // master waits for `parked` and only then sends INIT, so the INIT can never
    // strand a lock this core was holding (which would deadlock everyone else).
    if crate::percpu::claim_park_request() {
        crate::percpu::set_parked();
        loop {
            // SAFETY: parking this core for a physical offline.  Interrupts are
            // masked so that no lock is ever taken between the `parked` store
            // above and the INIT that resets the core.
            unsafe {
                core::arch::asm!("cli; hlt", options(nomem, nostack));
            }
        }
    }

    // Execute the selected depth.  With MONITOR/MWAIT and a known hint, enter
    // the state directly (C1 = hint 0; deeper states carry their `_CST` hint);
    // otherwise fall back to HLT, which is C1.
    let hint = cstates()[target as usize].mwait_hint;
    // SAFETY: `cpu_has_monitor` is a value-only CPUID accessor.
    let has_mwait = unsafe { ffi::cpu_has_monitor() } != 0;
    if hint >= 0 && has_mwait {
        mwait_idle(hint as u32);
    } else {
        hlt();
    }
    0
}

// ---------------------------------------------------------------------------
// IPI protocol (master -> worker)
// ---------------------------------------------------------------------------

/// Install the IPI_HALT / IPI_WAKEUP IDT gates. Call once at boot, after the
/// IDT is loaded (the two vectors replace stray-guard gates 0xF8/0xF9).
#[no_mangle]
pub extern "C" fn energy_ipi_init() -> i32 {
    if IPI_INIT_DONE.swap(true, core::sync::atomic::Ordering::SeqCst) {
        return 0;
    }
    let halt = (ipi_halt_isr as unsafe extern "C" fn()) as usize as u32;
    let wake = (ipi_wake_isr as unsafe extern "C" fn()) as usize as u32;
    // SAFETY: `IPI_INIT_DONE` guarantees this runs once, during boot after the IDT is loaded;
    // `IPI_HALT_VECTOR` is a valid IDT index and `halt` is the matching ISR stub.
    unsafe { ffi::set_idt_gate(IPI_HALT_VECTOR as i32, halt) };
    // SAFETY: as above, for the wake vector and its matching ISR stub.
    unsafe { ffi::set_idt_gate(IPI_WAKE_VECTOR as i32, wake) };
    0
}

fn ipi_send(dst_cpu: u32, vector: u32) -> i32 {
    if (dst_cpu as usize) >= MAX_CORES {
        return -1;
    }
    if energy::energy_core_role(dst_cpu) != 2 {
        // Only workers are driven by master IPIs.
        return -1;
    }
    if energy::energy_core_is_online(dst_cpu) == 0 {
        return -1;
    }

    let lapic_id = energy::energy_core_lapic_id(dst_cpu);
    if lapic_id == 0xFFFF_FFFF {
        return -1;
    }

    // The C side owns the ICR encoding (two MMIO registers in xAPIC mode,
    // one 64-bit MSR in x2APIC mode).
    // SAFETY: `lapic_id` has been checked for the invalid sentinel above and `vector` is one of
    // this controller's own IDT vectors; `apic_send_ipi` takes them by value and does the ICR
    // register/MSR write itself.
    unsafe { ffi::apic_send_ipi(lapic_id, vector) }
}

/// Ask worker `cpu` to (re-)enter its deepest eligible idle state.
#[no_mangle]
pub extern "C" fn energy_ipi_halt_worker(cpu: u32) -> i32 {
    ipi_send(cpu, IPI_HALT_VECTOR)
}

/// Wake worker `cpu` out of deep idle.
#[no_mangle]
pub extern "C" fn energy_ipi_wake_worker(cpu: u32) -> i32 {
    ipi_send(cpu, IPI_WAKE_VECTOR)
}

// ---------------------------------------------------------------------------
// IPI dispatch (invoked from the asm stubs in device_isrs.asm)
// ---------------------------------------------------------------------------

/// Worker received IPI_HALT: it should drop into its target idle depth on the
/// next idle pass. Harmless no-op until workers exist (UP build).
#[no_mangle]
pub extern "C" fn energy_ipi_halt_handle() {
    let _ = energy::energy_core_count_online();
}

/// Worker received IPI_WAKEUP: leave deep idle and rescan runqueues. Harmless
/// no-op until workers exist (UP build).
#[no_mangle]
pub extern "C" fn energy_ipi_wake_handle() {
    let _ = energy::energy_core_count_online();
}
