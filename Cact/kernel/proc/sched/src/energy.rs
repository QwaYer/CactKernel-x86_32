//! Master/Worker core roles and the per-core C-state model.
//!
//! Foundation (Step 1) of the energy-aware scheduler extension: every logical
//! core is described by a role (BSP = master, the rest = workers), its current
//! C-state and its idle/busy state.  The table is guarded by [`ENERGY_LOCK`]
//! and exported to the C kernel through the ABI in `Cact/kernel/energy/energy.h`.
//! Worker cores reported by the ACPI MADT are recorded as present but offline
//! (C6) until an SMP bring-up path starts them via [`energy_core_online`].

use core::cell::SyncUnsafeCell;
use core::ptr;

use crate::ffi;
use crate::sync::irq_spinlock_t;
use crate::sync::{irq_spinlock_acquire, irq_spinlock_release};

pub const MAX_CORES: usize = 64;
pub const MASTER_CPU: usize = 0;

const LAPIC_ID_INVALID: u32 = 0xFFFF_FFFF;
const IDLE_TASK_PID: u32 = 0;

#[derive(Copy, Clone, PartialEq)]
#[repr(u32)]
enum Role {
    None = 0,
    Master = 1,
    Worker = 2,
}

#[derive(Copy, Clone, PartialEq)]
#[repr(u32)]
enum CState {
    C0 = 0,
    C1 = 1,
    C3 = 2,
    C6 = 3,
}

impl CState {
    fn from_u32(v: u32) -> Option<CState> {
        match v {
            0 => Some(CState::C0),
            1 => Some(CState::C1),
            2 => Some(CState::C3),
            3 => Some(CState::C6),
            _ => None,
        }
    }
}

#[derive(Copy, Clone)]
struct EnergyCore {
    lapic_id: u32,
    role: Role,
    cstate: CState,
    is_idle: bool,
    idle_since: u32,
    present: bool,
    online: bool,
}

impl EnergyCore {
    const fn empty() -> Self {
        Self {
            lapic_id: LAPIC_ID_INVALID,
            role: Role::None,
            cstate: CState::C6,
            is_idle: false,
            idle_since: 0,
            present: false,
            online: false,
        }
    }
}

struct EnergyState {
    cores: [EnergyCore; MAX_CORES],
    present_count: u32,
    online_count: u32,
    initialized: bool,
}

impl EnergyState {
    const fn new() -> Self {
        Self {
            cores: [EnergyCore::empty(); MAX_CORES],
            present_count: 0,
            online_count: 0,
            initialized: false,
        }
    }
}

static ENERGY_STATE: SyncUnsafeCell<EnergyState> = SyncUnsafeCell::new(EnergyState::new());
static mut ENERGY_LOCK: irq_spinlock_t = irq_spinlock_t::new();

fn state() -> &'static mut EnergyState {
    // SAFETY: `ENERGY_STATE` is the governor's private global; every caller of `state()` holds
    // `ENERGY_LOCK` (see `lock`/`unlock` and `query`), so the reference is exclusive for the
    // duration of that critical section.
    unsafe { &mut *ENERGY_STATE.get() }
}

fn lock() {
    // SAFETY: `ENERGY_LOCK` is a statically initialised `irq_spinlock_t` at a unique address, so
    // `&raw mut ENERGY_LOCK` is a valid, aligned pointer to live lock storage.
    unsafe { irq_spinlock_acquire(&raw mut ENERGY_LOCK) };
}

fn unlock() {
    // SAFETY: pairs with the `lock()` above on the same statically initialised `ENERGY_LOCK`,
    // which this call site holds.
    unsafe { irq_spinlock_release(&raw mut ENERGY_LOCK) };
}

fn core_index(cpu: u32) -> Option<usize> {
    if (cpu as usize) < MAX_CORES {
        Some(cpu as usize)
    } else {
        None
    }
}

// ---------------------------------------------------------------------------
// ACPI MADT processor enumeration (only the entry walk lives here; ACPICA
// itself is the C ACPI driver).
// ---------------------------------------------------------------------------

const AE_OK: u32 = 0;
const MADT_TYPE_LOCAL_APIC: u8 = 0;
const MADT_TYPE_LOCAL_X2APIC: u8 = 9;
const MADT_ENABLED: u32 = 1;
const MADT_LOCAL_APIC_LEN: usize = 8;
// Processor Local x2APIC: 2 + 2 reserved + 4 x2APIC ID + 4 flags + 4 UID.
const MADT_LOCAL_X2APIC_LEN: usize = 16;
// ACPI_TABLE_MADT = table header (36) + UINT32 Address + UINT32 Flags (44).
const MADT_HEADER_LEN: usize = 44;

unsafe extern "C" {
    fn AcpiGetTable(signature: *const u8, instance: u32, out_table: *mut *mut u8) -> u32;
}

unsafe fn enumerate_madt_workers(st: &mut EnergyState, bsp_lapic: u32) {
    let mut table: *mut u8 = ptr::null_mut();
    // SAFETY: `AcpiGetTable` fills `table` and returns AE_OK on success; ACPI is initialised by
    // the time this runs.
    let rc = unsafe { AcpiGetTable(c"APIC".as_ptr().cast(), 1, &mut table) };
    if rc != AE_OK || table.is_null() {
        return;
    }

    let base = table as *const u8;
    // SAFETY: the MADT header is at least 8 bytes, so the length word at offset 4 is in bounds.
    let length_p = unsafe { base.add(4) } as *const u32;
    // SAFETY: `length_p` is a valid, possibly-unaligned pointer inside the table.
    let length = unsafe { ptr::read_unaligned(length_p) } as usize;
    // SAFETY: `apic_x2apic_mode` reports the boot-selected APIC mode and has no preconditions.
    let x2apic = unsafe { ffi::apic_x2apic_mode() };
    let mut off = MADT_HEADER_LEN;

    while off + 2 <= length && (st.present_count as usize) < MAX_CORES {
        // SAFETY: the loop guard `off + 2 <= length` keeps this byte inside the table.
        let sub_type_p = unsafe { base.add(off) };
        // SAFETY: `sub_type_p` is that in-bounds byte.
        let sub_type = unsafe { *sub_type_p };
        // SAFETY: as above, the second byte of the sub-entry header is in bounds.
        let sub_len_p = unsafe { base.add(off + 1) };
        // SAFETY: `sub_len_p` is that in-bounds byte.
        let sub_len = unsafe { *sub_len_p } as usize;
        if sub_len == 0 {
            break;
        }

        /* Both encodings may be present for the same processor; take the id
         * and flags from whichever this entry carries. */
        let entry = if sub_type == MADT_TYPE_LOCAL_APIC && sub_len >= MADT_LOCAL_APIC_LEN {
            // SAFETY: `sub_len >= MADT_LOCAL_APIC_LEN` (8), so the id byte and flags word lie
            // inside this sub-entry.
            let id_p = unsafe { base.add(off + 3) };
            // SAFETY: `id_p` is that in-bounds byte.
            let id = unsafe { *id_p } as u32;
            // SAFETY: `off + 4` is inside the 8-byte sub-entry.
            let flags_p = unsafe { base.add(off + 4) } as *const u32;
            // SAFETY: `flags_p` is inside the sub-entry and may be unaligned.
            let flags = unsafe { ptr::read_unaligned(flags_p) };
            Some((id, flags))
        } else if sub_type == MADT_TYPE_LOCAL_X2APIC && sub_len >= MADT_LOCAL_X2APIC_LEN {
            // SAFETY: `sub_len >= MADT_LOCAL_X2APIC_LEN` (16), so the id and flags words lie
            // inside this sub-entry.
            let id_p = unsafe { base.add(off + 4) } as *const u32;
            // SAFETY: `id_p` is inside the sub-entry and may be unaligned.
            let id = unsafe { ptr::read_unaligned(id_p) };
            // SAFETY: `off + 8` is likewise inside the 16-byte sub-entry.
            let flags_p = unsafe { base.add(off + 8) } as *const u32;
            // SAFETY: `flags_p` is inside the sub-entry and may be unaligned.
            let flags = unsafe { ptr::read_unaligned(flags_p) };
            Some((id, flags))
        } else {
            None
        };
        off += sub_len;

        let (apic_id, flags) = match entry {
            Some(e) => e,
            None => continue,
        };
        if (flags & MADT_ENABLED) == 0 || apic_id == bsp_lapic {
            continue;
        }
        /* xAPIC addresses 8-bit ids only, so a processor that is described
         * solely by a >255 x2APIC entry cannot be woken in that mode. */
        if !x2apic && apic_id > 0xFF {
            continue;
        }
        /* The two entry types carry the same id for ids < 256 — count the
         * processor once. */
        let slot = st.present_count as usize;
        if st.cores[..slot].iter().any(|c| c.lapic_id == apic_id) {
            continue;
        }

        let worker = &mut st.cores[slot];
        worker.lapic_id = apic_id;
        worker.role = Role::Worker;
        worker.cstate = CState::C6;
        worker.present = true;
        worker.online = false;
        st.present_count += 1;
    }
}

// ---------------------------------------------------------------------------
// C ABI
// ---------------------------------------------------------------------------

#[no_mangle]
pub extern "C" fn energy_init() -> i32 {
    lock();

    let st = state();
    if !st.initialized {
        for c in st.cores.iter_mut() {
            *c = EnergyCore::empty();
        }
        st.present_count = 0;
        st.online_count = 0;

        // SAFETY: `apic_lapic_id` is a leaf C helper that reads this CPU's LAPIC ID; it takes no
        // pointers and returns a plain value.
        let bsp_lapic = unsafe { ffi::apic_lapic_id() };

        let master = &mut st.cores[MASTER_CPU];
        master.lapic_id = bsp_lapic;
        master.role = Role::Master;
        master.cstate = CState::C0;
        master.present = true;
        master.online = true;
        st.present_count = 1;
        st.online_count = 1;

        if ffi::acpi_available() != 0 {
            // SAFETY: `st` was just reset above and is still exclusively borrowed here;
            // `enumerate_madt_workers` documents that it only walks the MADT with explicit
            // length/sub-entry bounds checks before each read.
            unsafe { enumerate_madt_workers(st, bsp_lapic) };
        }

        st.initialized = true;
    }

    unlock();
    0
}

fn query<T>(f: impl FnOnce(&EnergyState) -> T) -> T {
    lock();
    let st = state();
    let out = f(st);
    unlock();
    out
}

#[no_mangle]
pub extern "C" fn energy_core_count_present() -> u32 {
    query(|st| st.present_count)
}

#[no_mangle]
pub extern "C" fn energy_core_count_online() -> u32 {
    query(|st| st.online_count)
}

#[no_mangle]
pub extern "C" fn energy_master_cpu() -> u32 {
    MASTER_CPU as u32
}

#[no_mangle]
pub extern "C" fn energy_core_lapic_id(cpu: u32) -> u32 {
    match core_index(cpu) {
        Some(i) => query(|st| st.cores[i].lapic_id),
        None => LAPIC_ID_INVALID,
    }
}

#[no_mangle]
pub extern "C" fn energy_core_role(cpu: u32) -> u32 {
    match core_index(cpu) {
        Some(i) => query(|st| st.cores[i].role as u32),
        None => Role::None as u32,
    }
}

#[no_mangle]
pub extern "C" fn energy_core_cstate(cpu: u32) -> u32 {
    match core_index(cpu) {
        Some(i) => query(|st| st.cores[i].cstate as u32),
        None => CState::C6 as u32,
    }
}

#[no_mangle]
pub extern "C" fn energy_core_is_idle(cpu: u32) -> i32 {
    core_index(cpu)
        .is_some_and(|i| query(|st| st.cores[i].is_idle)) as i32
}

#[no_mangle]
pub extern "C" fn energy_core_is_present(cpu: u32) -> i32 {
    core_index(cpu)
        .is_some_and(|i| query(|st| st.cores[i].present)) as i32
}

#[no_mangle]
pub extern "C" fn energy_core_is_online(cpu: u32) -> i32 {
    core_index(cpu)
        .is_some_and(|i| query(|st| st.cores[i].online)) as i32
}

#[no_mangle]
pub extern "C" fn energy_core_is_master(cpu: u32) -> i32 {
    core_index(cpu)
        .is_some_and(|i| query(|st| st.cores[i].role == Role::Master)) as i32
}

#[no_mangle]
pub extern "C" fn energy_core_is_worker(cpu: u32) -> i32 {
    core_index(cpu)
        .is_some_and(|i| query(|st| st.cores[i].role == Role::Worker)) as i32
}

#[no_mangle]
pub extern "C" fn energy_core_idle_since_tick(cpu: u32) -> u32 {
    match core_index(cpu) {
        Some(i) => query(|st| st.cores[i].idle_since),
        None => 0,
    }
}

#[no_mangle]
pub extern "C" fn energy_core_set_cstate(cpu: u32, state_value: u32) -> i32 {
    let Some(idx) = core_index(cpu) else { return -1; };
    let Some(cstate) = CState::from_u32(state_value) else {
        return -1;
    };

    lock();

    let st = state();
    let core = &mut st.cores[idx];
    let master_ok = idx != MASTER_CPU || cstate == CState::C0 || cstate == CState::C1;
    let rc = if !core.online || !master_ok {
        -1
    } else {
        core.cstate = cstate;
        0
    };

    unlock();
    rc
}

#[no_mangle]
pub extern "C" fn energy_core_mark_idle(cpu: u32) {
    let Some(idx) = core_index(cpu) else { return };

    lock();

    let st = state();
    if !st.initialized {
        unlock();
        return;
    }
    let core = &mut st.cores[idx];
    if core.online && !core.is_idle {
        core.is_idle = true;
        core.idle_since = ffi::timer_ticks_get();
    }

    unlock();
}

#[no_mangle]
pub extern "C" fn energy_core_mark_busy(cpu: u32) {
    let Some(idx) = core_index(cpu) else { return };

    lock();

    let st = state();
    if !st.initialized {
        unlock();
        return;
    }
    let core = &mut st.cores[idx];
    if core.online {
        core.is_idle = false;
        core.idle_since = 0;
        // A core that is about to run work must be fully active.
        core.cstate = CState::C0;
    }

    unlock();
}

#[no_mangle]
pub extern "C" fn energy_core_online(cpu: u32, lapic_id: u32) -> i32 {
    let Some(idx) = core_index(cpu) else { return -1 };

    lock();

    let st = state();
    if !st.initialized {
        unlock();
        return -1;
    }

    let core = &mut st.cores[idx];
    if !core.present {
        core.present = true;
        st.present_count += 1;
    }
    core.lapic_id = lapic_id;
    if core.role == Role::None {
        core.role = if idx == MASTER_CPU { Role::Master } else { Role::Worker };
    }
    if !core.online {
        core.online = true;
        st.online_count += 1;
    }
    core.cstate = CState::C0;
    core.is_idle = false;
    core.idle_since = 0;

    unlock();
    0
}

#[no_mangle]
pub extern "C" fn energy_core_offline(cpu: u32) {
    let Some(idx) = core_index(cpu) else { return };
    if idx == MASTER_CPU {
        return;
    }

    lock();

    let st = state();
    if !st.initialized {
        unlock();
        return;
    }
    let core = &mut st.cores[idx];
    if core.online {
        core.online = false;
        st.online_count -= 1;
    }
    core.cstate = CState::C6;
    core.is_idle = false;
    core.idle_since = 0;

    unlock();
}

// ---------------------------------------------------------------------------
// Scheduler integration
// ---------------------------------------------------------------------------

/// Called from `schedule()` once the next task has been chosen.  The idle
/// task (pid 0) marks its core idle, any real task marks it busy.  Runs with
/// the scheduler lock held (IRQs off).
pub(crate) fn observe_schedule(cpu: u32, next_pid: u32) {
    if next_pid == IDLE_TASK_PID {
        energy_core_mark_idle(cpu);
    } else {
        energy_core_mark_busy(cpu);
    }
}
