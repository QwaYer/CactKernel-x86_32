//! SMP bring-up (Step A of the energy plan): activate worker cores.
//!
//! The master (BSP) stages a real-mode trampoline in low memory, builds a
//! per-CPU GDT/TSS/idle-stack environment for every present core and wakes
//! the workers with an INIT-SIPI-SIPI sequence.  Each AP runs
//! [`smp_ap_entry`], loads its per-CPU environment, marks itself online in
//! the energy governor and drops into the C-state idle loop.
//!
//! The trampoline is a raw binary object (`trampoline.bin`) embedded through
//! `ld -r -b binary`; only its info block (cr3/stack/entry) is patched here.

use core::cell::SyncUnsafeCell;
use core::ptr;

use crate::energy;
use crate::cstate;
use crate::ffi;
use crate::percpu;
use crate::sync::{irq_spinlock_acquire, irq_spinlock_release};

const MAX_CPUS: usize = 64;
const TSS_SLOT: usize = 5;

// Copy destination + SIPI vector of the trampoline.
const TRAMP_ADDR: u32 = 0x8000;
const TRAMP_VECTOR: u32 = TRAMP_ADDR >> 12;

/// Enables worker task execution: when true, each AP arms its LAPIC timer and
/// calls `schedule()`, so tasks may run on any core.  Set false to fall back to
/// BSP-only scheduling (the system then behaves like a uniprocessor build).
///
/// The scheduler's block/wake protocol, the kernel log and the console were
/// made SMP-safe before this could be turned back on: a waker only queues a
/// task that has actually parked (otherwise the task's own park step queues
/// it), `klog_emit` and the console take real spinlocks.  Flip to false only to
/// bisect an SMP problem.
const WORKERS_RUN_TASKS: bool = true;
// Info block offsets inside the trampoline page (INFO_BASE in trampoline.asm).
// The vector lands an AP on offset 0, which trampoline.asm must keep as entry
// code, so the BSP-patched block sits at a fixed offset near the top instead.
const INFO_CR3: usize = 0x0F00;
const INFO_STACK: usize = 0x0F04;
const INFO_ENTRY: usize = 0x0F08;
const INFO_CPU: usize = 0x0F0C;
// AP sets INFO_ACK = AP_STAGE_TRAMP_READ once it consumed the info block.
// The BSP must not re-stamp 0x8000 for the next worker until it is set (or
// the worker is confirmed lost), otherwise the AP could read a stale/mixed
// stack/cpu pair while still executing from the shared page.
const INFO_ACK: usize = 0x0F10;
const AP_STAGE_TRAMP_READ: u32 = 1;

/// Bring-up / idle stack per worker.  A worker runs its whole idle life on this
/// stack (the trampoline loads it as ESP and the idle task keeps it), so the
/// tick, the governor and any console output they do nest here: 16 KB leaves
/// real headroom for that.
const IDLE_STACK_SIZE: usize = 16384;

// INIT-SIPI-SIPI timing: 10 ms after INIT, 1 ms between the two SIPIs.
const INIT_SIPI_DELAY_MS: u32 = 10;
const SIPI_GAP_MS: u32 = 1;

// ---------------------------------------------------------------------------
// Hardware-layout structs (packed mirrors of the C boot GDT/TSS).
// ---------------------------------------------------------------------------

#[repr(C, packed)]
#[derive(Copy, Clone)]
struct GdtEntry {
    limit_low: u16,
    base_low: u16,
    base_middle: u8,
    access: u8,
    granularity: u8,
    base_high: u8,
}

#[repr(C, packed)]
#[derive(Copy, Clone)]
struct GdtPtr {
    limit: u16,
    base: u32,
}

#[repr(C, packed)]
#[derive(Copy, Clone)]
struct Tss {
    prev_tss: u32,
    esp0: u32,
    ss0: u32,
    esp1: u32,
    ss1: u32,
    esp2: u32,
    ss2: u32,
    cr3: u32,
    eip: u32,
    eflags: u32,
    eax: u32,
    ecx: u32,
    edx: u32,
    ebx: u32,
    esp: u32,
    ebp: u32,
    esi: u32,
    edi: u32,
    es: u32,
    cs: u32,
    ss: u32,
    ds: u32,
    fs: u32,
    gs: u32,
    ldt: u32,
    trap: u16,
    iomap_base: u16,
}

#[repr(C, align(16))]
struct SmpCpu {
    gdt: [GdtEntry; TSS_SLOT + MAX_CPUS],
    gp: GdtPtr,
    tss: Tss,
    idle_stack: [u8; IDLE_STACK_SIZE],
    lapic_id: u32,
    online: u32,
}

impl SmpCpu {
    const fn empty() -> Self {
        Self {
            gdt: [GdtEntry {
                limit_low: 0,
                base_low: 0,
                base_middle: 0,
                access: 0,
                granularity: 0,
                base_high: 0,
            }; TSS_SLOT + MAX_CPUS],
            gp: GdtPtr { limit: 0, base: 0 },
            tss: Tss {
                prev_tss: 0,
                esp0: 0,
                ss0: 0,
                esp1: 0,
                ss1: 0,
                esp2: 0,
                ss2: 0,
                cr3: 0,
                eip: 0,
                eflags: 0,
                eax: 0,
                ecx: 0,
                edx: 0,
                ebx: 0,
                esp: 0,
                ebp: 0,
                esi: 0,
                edi: 0,
                es: 0,
                cs: 0,
                ss: 0,
                ds: 0,
                fs: 0,
                gs: 0,
                ldt: 0,
                trap: 0,
                iomap_base: 0,
            },
            idle_stack: [0; IDLE_STACK_SIZE],
            lapic_id: 0,
            online: 0,
        }
    }
}

static CPU_TABLE: SyncUnsafeCell<[SmpCpu; MAX_CPUS]> =
    SyncUnsafeCell::new([const { SmpCpu::empty() }; MAX_CPUS]);
static mut SMP_READY: bool = false;


/// Read a worker's raw online flag (no energy locks).
#[no_mangle]
pub extern "C" fn smp_cpu_online(cpu: u32) -> i32 {
    if (cpu as usize) >= MAX_CPUS {
        return 0;
    }
    let p = cpu_ptr(cpu as usize);
    // SAFETY: `cpu` was bounds-checked against `MAX_CPUS` above, so `cpu_ptr` returns a pointer
    // inside the `CPU_TABLE` array and the `online` field read is in bounds.
    unsafe { (*p).online as i32 }
}


unsafe extern "C" {
    #[link_name = "_binary_build_trampoline_bin_start"]
    static TRAMPOLINE_BIN_START: u8;
    #[link_name = "_binary_build_trampoline_bin_end"]
    static TRAMPOLINE_BIN_END: u8;
}

fn cpu_ptr(cpu: usize) -> *mut SmpCpu {
    let base = CPU_TABLE.get() as *mut SmpCpu;
    // SAFETY: `base` addresses the statically allocated `CPU_TABLE`; every caller bounds-checks
    // `cpu` against `MAX_CPUS`, so the offset stays inside that array.
    unsafe { base.add(cpu) }
}

fn delay_ms(ms: u32) {
    let mut n = ms.saturating_mul(20000);
    while n > 0 {
        n -= 1;
        // SAFETY: `pause` is a spin-wait hint; it touches no memory, no registers and no flags.
        unsafe {
            core::arch::asm!("pause", options(nomem, nostack, preserves_flags));
        }
    }
}

fn read_cr3() -> u32 {
    let cr3: u32;
    // SAFETY: `mov ..., cr3` only copies the current page-directory base into an output register;
    // `nomem`/`nostack` declare that it reads and writes no memory.
    unsafe {
        core::arch::asm!("mov {0}, cr3", out(reg) cr3, options(nomem, nostack));
    }
    cr3
}

fn stack_top(cpu: usize) -> u32 {
    let p = cpu_ptr(cpu);
    // SAFETY: `p` points at a `CPU_TABLE` entry for a bounds-checked `cpu`, so `idle_stack` is a
    // live in-bounds array and taking its address is valid.
    let base = unsafe { (&raw mut (*p).idle_stack) as usize };
    // Round *down* so the whole stack stays inside `idle_stack`.  Rounding the
    // top *up* used to start the stack a few bytes past the array, on top of the
    // fields that follow it (`lapic_id`, `online`) — the fix for that is here,
    // because it made every worker's recorded LAPIC id garbage and the re-online
    // path then INIT'd APIC id 0, the BSP, which resets the machine.
    ((base + IDLE_STACK_SIZE) & !15usize) as u32
}

fn write_volatile<T: Copy>(addr: *mut T, val: T) {
    // SAFETY: the caller guarantees `addr` points to a valid, aligned, live `T`; a volatile store
    // performs exactly that write and nothing else.
    unsafe { addr.write_volatile(val) };
}

// ---------------------------------------------------------------------------
// Per-CPU GDT/TSS environment
// ---------------------------------------------------------------------------

fn set_gdt(entry: &mut GdtEntry, base: u32, limit: u32, access: u8, gran: u8) {
    entry.base_low = (base & 0xFFFF) as u16;
    entry.base_middle = ((base >> 16) & 0xFF) as u8;
    entry.base_high = ((base >> 24) & 0xFF) as u8;
    entry.limit_low = (limit & 0xFFFF) as u16;
    entry.granularity = (((limit >> 16) & 0x0F) as u8) | (gran & 0xF0);
    entry.access = access;
}

/// (Re)write `cpu`'s TSS descriptor in its per-CPU GDT.
///
/// `ltr` marks the descriptor *busy* (type 0xB) and the processor refuses to
/// load a busy TSS with #GP.  The bit is never cleared, so a core that is
/// brought up a second time (the physical re-online path) must have its
/// descriptor rebuilt first — otherwise `smp_ap_entry`'s `ltr` faults before the
/// AP has an IDT, which becomes a triple fault and resets the machine.
fn install_tss_descriptor(p: &mut SmpCpu, cpu: usize) {
    let tss_base = (&raw mut p.tss) as u32;
    set_gdt(
        &mut p.gdt[TSS_SLOT + cpu],
        tss_base,
        core::mem::size_of::<Tss>() as u32 - 1,
        0xE9,
        0x00,
    );
}

fn build_cpu_env(cpu: usize, lapic_id: u32) {
    let p = cpu_ptr(cpu);
    let count = TSS_SLOT + MAX_CPUS;

    // SAFETY: `p` points at the `CPU_TABLE` entry for `cpu` (bounds-checked by the caller), so
    // reborrowing it exclusively here, during single-threaded BSP bring-up before the AP starts,
    // is sound.
    let p = unsafe { &mut *p };

    // Reset descriptors (all-zero except the ones we set).
    for slot in p.gdt[..count].iter_mut() {
        write_volatile(slot as *mut GdtEntry, GdtEntry {
            limit_low: 0,
            base_low: 0,
            base_middle: 0,
            access: 0,
            granularity: 0,
            base_high: 0,
        });
    }

    // Fixed slots 0..4 mirror the boot GDT (user selectors included).
    set_gdt(&mut p.gdt[0], 0, 0, 0, 0);
    set_gdt(&mut p.gdt[1], 0, 0xFFFF_FFFF, 0x9A, 0xCF);
    set_gdt(&mut p.gdt[2], 0, 0xFFFF_FFFF, 0x92, 0xCF);
    set_gdt(&mut p.gdt[3], 0, 0xFFFF_FFFF, 0xFA, 0xCF);
    set_gdt(&mut p.gdt[4], 0, 0xFFFF_FFFF, 0xF2, 0xCF);

    // Per-CPU TSS at slot TSS_SLOT + cpu (str() identifies the CPU).
    install_tss_descriptor(p, cpu);

    p.tss.ss0 = 0x10;
    p.tss.esp0 = stack_top(cpu);
    p.tss.iomap_base = core::mem::size_of::<Tss>() as u16;

    p.gp.limit = (count * core::mem::size_of::<GdtEntry>()) as u16 - 1;
    p.gp.base = (&raw mut p.gdt) as u32;
    p.lapic_id = lapic_id;
    write_volatile(&raw mut p.online, 0);
}

fn read_info(off: usize) -> u32 {
    // SAFETY: the trampoline binary was copied to the identity-mapped `TRAMP_ADDR` page, and
    // `off` is one of the fixed `INFO_*` offsets inside that page, so the volatile word read is
    // in bounds.
    unsafe { ((TRAMP_ADDR as usize + off) as *const u32).read_volatile() }
}


// ---------------------------------------------------------------------------
// AP entry
// ---------------------------------------------------------------------------

/// Trampoline target: runs on a worker's own idle stack, in protected mode
/// with paging enabled. The cpu index is passed by the BSP through the
/// trampoline info block; no energy/per-CPU state is touched until the
/// per-CPU GDT/TSS and shared IDT are installed.
#[no_mangle]
pub extern "C" fn smp_ap_entry() -> ! {
    let cpu = read_info(INFO_CPU) as usize;
    // Acknowledge that the trampoline info block has been consumed so the BSP
    // may safely re-stamp 0x8000 for the next worker.
    write_volatile((TRAMP_ADDR as usize + INFO_ACK) as *mut u32, AP_STAGE_TRAMP_READ);
    if cpu >= MAX_CPUS {
        loop {
            // SAFETY: an out-of-range CPU id means the trampoline was fed bad info; `cli; hlt`
            // touches no memory and parks this AP forever, which is all that is left to do.
            unsafe {
                core::arch::asm!("cli; hlt", options(nomem, nostack));
            }
        }
    }

    let p = cpu_ptr(cpu);
    let sel = ((TSS_SLOT + cpu) << 3) as u16;
    // SAFETY: the AP runs on its own `idle_stack` and `cpu < MAX_CPUS` (checked above), so `p`
    // is this CPU's own live `CPU_TABLE` entry, exclusively reborrowed before it is brought up.
    let p = unsafe { &mut *p };

    // SAFETY: loading the per-CPU GDT just built for this CPU is the documented AP bring-up step.
    unsafe { ffi::gdt_flush((&raw const p.gp) as u32) };
    // SAFETY: `sel` is this CPU's TSS selector (`TSS_SLOT + cpu`) and that TSS is initialised.
    unsafe { core::arch::asm!("ltr {0:x}", in(reg) sel, options(nomem, nostack)) };
    // SAFETY: the shared kernel IDT is already built and loaded on the BSP; reloading it makes
    // this AP use it too.
    unsafe { ffi::idt_reload() };

    // Per-CPU CPU setup the BSP does once at boot.  These touch *per-core*
    // registers/MSRs, so every core must do them itself; skipping them is what
    // made `-smp 4` fault with #UD (SIGILL) the moment ring-3 code ran an SSE
    // instruction (CR4.OSFXSR was set only on the BSP).
    // SAFETY: each call programs only the calling CPU's own registers/MSRs.
    let _ = unsafe { ffi::fpu_global_init() };
    // SAFETY: as above, for the calling CPU's IA32_PAT MSR.
    unsafe { ffi::pat_init() };
    // SAFETY: as above, for the calling CPU's fast-syscall MSRs.
    let _ = unsafe { ffi::cpu_syscall_commit() };

    // SAFETY: brings this AP online in the APIC layer; its LAPIC is mapped and enabled.
    unsafe { ffi::apic_ap_online() };

    // Adopt this CPU's idle task as its running context.  `this_cpu()` resolves
    // correctly now that this AP's per-CPU TSS is loaded.
    percpu::set_current(percpu::idle());
    // A fresh bring-up (including a re-online after a physical offline) clears
    // any leftover park request/flag for this core.
    percpu::clear_park_state(cpu as usize);

    // Online in the energy governor and idle from the start.
    // SAFETY: reads this CPU's own LAPIC id; the LAPIC is mapped and enabled.
    let lapic_id = unsafe { ffi::apic_lapic_id() };
    let _ = energy::energy_core_online(cpu as u32, lapic_id);
    energy::energy_core_mark_idle(cpu as u32);
    write_volatile(&raw mut p.online, 1);

    // SAFETY: enables interrupts on this AP now that its GDT/TSS/IDT are installed.
    unsafe { core::arch::asm!("sti", options(nomem, nostack)) };

    if WORKERS_RUN_TASKS {
        // Arm this core's own LAPIC timer at the BSP-calibrated rate so the
        // scheduler tick and local preemption work here too.  (The BSP arms itself
        // in apic_init(), which runs before bring-up, so the rate is ready.)
        // SAFETY: `lapic_timer_ticks_per_ms` is a value-only getter; the LAPIC is
        // mapped and enabled on this CPU (apic_ap_online above).
        let per_ms = unsafe { ffi::lapic_timer_ticks_per_ms() };
        if per_ms != 0 {
            // SAFETY: `per_ms` is the calibrated rate and this programs the calling
            // CPU's own LAPIC timer.
            unsafe { ffi::lapic_timer_start_periodic(per_ms) };
        }
    }

    // Worker loop.  With WORKERS_RUN_TASKS the core runs whatever the governor
    // has made runnable and otherwise halts in its target C-state until the next
    // tick or a wake IPI; without it, the core just idles (no timer, no
    // scheduling) and the system runs on the BSP alone.
    loop {
        if WORKERS_RUN_TASKS {
            // SAFETY: `schedule` is the scheduler switch routine; this CPU's
            // GDT/TSS/IDT and per-CPU scheduler state are installed.
            unsafe { crate::mlfq::schedule() };
        }
        let _ = cstate::energy_cstate_idle(cpu as u32);
    }
}

// ---------------------------------------------------------------------------
// Master (BSP) side
// ---------------------------------------------------------------------------

fn stage_trampoline() -> i32 {
    let start = (&raw const TRAMPOLINE_BIN_START) as usize;
    let end = (&raw const TRAMPOLINE_BIN_END) as usize;
    let len = end.wrapping_sub(start);
    if len == 0 || len > 0x1000 {
        return -1;
    }

    let dst = TRAMP_ADDR as *mut u8;
    // SAFETY: `TRAMPOLINE_BIN_START`/`_END` bracket the linked-in trampoline blob and `len` was
    // checked to be non-zero and at most one page, so source and destination (the identity-mapped
    // `TRAMP_ADDR` page) are valid, non-overlapping regions of `len` bytes.
    unsafe {
        ptr::copy_nonoverlapping(start as *const u8, dst, len);
    }
    // Patch info block.
    write_volatile((TRAMP_ADDR as usize + INFO_CR3) as *mut u32, read_cr3());
    write_volatile((TRAMP_ADDR as usize + INFO_STACK) as *mut u32, 0);
    write_volatile((TRAMP_ADDR as usize + INFO_CPU) as *mut u32, 0);
    write_volatile((TRAMP_ADDR as usize + INFO_ACK) as *mut u32, 0);
    let entry = (smp_ap_entry as extern "C" fn() -> !) as usize as u32;
    write_volatile((TRAMP_ADDR as usize + INFO_ENTRY) as *mut u32, entry);
    0
}

/// Wake every present worker core. Returns 0 on success (a core that fails
/// to come up is reported through the energy table as still offline).
#[no_mangle]
pub extern "C" fn smp_init() -> i32 {
    // SAFETY: `smp_init` runs once on the BSP during single-threaded boot bring-up, so nothing
    // else can be reading or writing the `SMP_READY` static concurrently.
    unsafe {
        if SMP_READY {
            return 0;
        }
    }

    let present = energy::energy_core_count_present() as usize;
    let present = present.min(MAX_CPUS);

    // Build per-CPU environments from the energy core map (single-threaded).
    for cpu in 0..present {
        build_cpu_env(cpu, energy::energy_core_lapic_id(cpu as u32));
        if cpu != 0 {
            // A fallback context for this worker, allocated on the BSP while
            // bring-up is still single-threaded.  The AP adopts it as its
            // running task in `smp_ap_entry`.
            // SAFETY: BSP bring-up is single-threaded here; `alloc_idle_task` only allocates
            // and initialises a task.
            let idle = unsafe { crate::task::alloc_idle_task() };
            percpu::set_idle_for(cpu, idle);
        }
    }

    if stage_trampoline() != 0 {
        return -1;
    }

    for cpu in 1..present {
        if smp_cpu_online(cpu as u32) != 0 {
            continue;
        }
        // Raw reads only (no energy locks): an AP must not be able to wedge
        // the BSP's bring-up loop.
        // SAFETY: `cpu` is below `present`, which was clamped to `MAX_CPUS`, so `cpu_ptr(cpu)`
        // addresses a live `CPU_TABLE` entry and the `lapic_id` read is in bounds.
        let lapic_id = unsafe { (*cpu_ptr(cpu)).lapic_id };

        // Stamp the shared info block for this worker.  INFO_ACK is cleared
        // first; we do NOT re-stamp 0x8000 for the next worker until this AP
        // has acked (consumed) the block.
        write_volatile((TRAMP_ADDR as usize + INFO_ACK) as *mut u32, 0);
        write_volatile(
            (TRAMP_ADDR as usize + INFO_STACK) as *mut u32,
            stack_top(cpu),
        );
        write_volatile((TRAMP_ADDR as usize + INFO_CPU) as *mut u32, cpu as u32);

        // INIT-SIPI-SIPI wakeup sequence with 10 ms after INIT and a 1 ms
        // gap between the two SIPIs.
        // SAFETY: `lapic_id` was read from this CPU's own `CPU_TABLE` entry, so it is that
        // worker's real LAPIC id; the C helpers do the ICR register/MSR writes themselves.
        unsafe {
            ffi::apic_send_init_ipi(lapic_id);
        }
        delay_ms(INIT_SIPI_DELAY_MS);
        // SAFETY: same LAPIC id as above, and `TRAMP_VECTOR` is the SIPI vector for the
        // identity-mapped `TRAMP_ADDR` trampoline page.
        unsafe {
            ffi::apic_send_sipi(lapic_id, TRAMP_VECTOR);
        }
        delay_ms(SIPI_GAP_MS);
        // SAFETY: the second SIPI is the same call as the first, per the INIT-SIPI-SIPI
        // sequence this bring-up uses.
        unsafe {
            ffi::apic_send_sipi(lapic_id, TRAMP_VECTOR);
        }

        // Phase 1 — AP_STAGE_TRAMP_READ handshake: the AP must consume the
        // info block (it acks right after read_info) before we ever touch the
        // shared 0x8000 page again.  Waiting on `online` alone is NOT enough:
        // online is set much later, so it must not be used as a proxy here.
        let mut ack_waits = 0;
        while read_info(INFO_ACK) != AP_STAGE_TRAMP_READ && ack_waits < 1000 {
            delay_ms(10);
            ack_waits += 1;
        }
        if read_info(INFO_ACK) != AP_STAGE_TRAMP_READ {
            // The AP never read the block: it either never started (SIPI
            // lost) or is stuck mid-trampoline.  Re-stamping 0x8000 now could
            // feed it the next worker's data, so stop waking further cores.
            break;
        }

        // Phase 2 — wait for the worker to finish bring-up and go online.
        let mut online_waits = 0;
        while smp_cpu_online(cpu as u32) == 0 && online_waits < 500 {
            delay_ms(10);
            online_waits += 1;
        }
    }

    // SAFETY: still on the BSP during single-threaded boot bring-up; this records that bring-up
    // has finished so a later call short-circuits.
    unsafe {
        SMP_READY = true;
    }
    0
}

/// True when `lapic` is not a usable AP destination: the invalid sentinel, or
/// this CPU's own LAPIC id.  `smp_cpu_offline` and `smp_cpu_online_sipi` always
/// run on the BSP, and an INIT delivered to the BSP resets the machine, so a
/// destination equal to the local LAPIC id must be refused.  Read the id from the
/// APIC itself: the core map's copy is only as trustworthy as the map.
fn lapic_is_bsp_or_invalid(lapic: u32) -> bool {
    if lapic == 0xFFFF_FFFF {
        return true;
    }
    // SAFETY: `apic_lapic_id` reads the calling CPU's own LAPIC id register only.
    lapic == unsafe { ffi::apic_lapic_id() }
}

/// Physically take worker `cpu` offline.
///
/// It is NOT safe to INIT a core that might be holding a global lock: its idle
/// path takes the energy lock and its tick the scheduler/monitor locks, and a
/// core reset mid-lock strands that lock forever, deadlocking every other core.
/// So we ask the core to park *itself* at a lock-free point
/// (`percpu::park_request` -> `parked`, observed in `energy_cstate_idle`), wait
/// for it to report parked, and only then send INIT.  Only an idle core with an
/// empty runqueue is asked, and the check runs under `SCHEDULER_LOCK`.
pub fn smp_cpu_offline(cpu: u32) -> i32 {
    if cpu == 0 || (cpu as usize) >= MAX_CPUS {
        return -1;
    }
    let lapic = energy::energy_core_lapic_id(cpu);
    if lapic_is_bsp_or_invalid(lapic) {
        return -1;
    }
    let idx = cpu as usize;

    // SAFETY: `SCHEDULER_LOCK` is the scheduler's global spinlock; this runs
    // from task context (the master's idle loop), which does not hold it.
    unsafe { irq_spinlock_acquire(&raw mut crate::task::SCHEDULER_LOCK) };
    let idle_task = crate::percpu::cpu_idle(idx);
    let safe = !idle_task.is_null()
        && crate::percpu::cpu_current(idx) == idle_task
        && crate::mlfq::mlfq_runnable_count() == 0;
    if safe {
        crate::percpu::set_park_request(idx);
    }
    // SAFETY: the lock was acquired above.
    unsafe { irq_spinlock_release(&raw mut crate::task::SCHEDULER_LOCK) };
    if !safe {
        return -1;
    }

    // Wait for the core to reach its parking point (it wakes on its next tick).
    let mut waits = 0;
    while crate::percpu::cpu_parked(idx) == 0 && waits < 20 {
        delay_ms(5);
        waits += 1;
    }
    if crate::percpu::cpu_parked(idx) == 0 {
        // Not parked yet.  If the core never claimed the request it went on to
        // run a task -> cancel and leave it alone.  If it did claim the request
        // it is about to park (and holds no lock), so the INIT below is safe.
        if crate::percpu::cancel_park_request(idx) {
            return -1;
        }
    }

    // The core is parked at a lock-free point, so INIT cannot strand a lock.
    // SAFETY: see above; the C helper does the ICR write itself.
    unsafe { ffi::apic_send_init_ipi(lapic) };
    // The info block is now free for a later wake: clearing the ACK here is what
    // lets `smp_cpu_online_sipi` tell "fresh core" from "previous wake died".
    write_volatile((TRAMP_ADDR as usize + INFO_ACK) as *mut u32, 0);
    crate::percpu::clear_park_state(idx);

    let p = cpu_ptr(idx);
    // SAFETY: `cpu` is bounds-checked above, so `p` addresses a live `CPU_TABLE`
    // entry and this clears that core's online flag.
    unsafe { write_volatile(&raw mut (*p).online, 0) };
    energy::energy_core_offline(cpu);
    0
}

/// Bring an offlined worker `cpu` back up by re-running the boot trampoline
/// sequence (INIT-SIPI-SIPI), reusing the info block still staged at
/// `TRAMP_ADDR`.  Returns 0 once the core reports online again.
pub fn smp_cpu_online_sipi(cpu: u32) -> i32 {
    if cpu == 0 || (cpu as usize) >= MAX_CPUS {
        return -1;
    }
    let p = cpu_ptr(cpu as usize);
    // SAFETY: `cpu` is bounds-checked above, so `p` addresses a live `CPU_TABLE`
    // entry and reading its LAPIC id is in bounds.
    let lapic = unsafe { (*p).lapic_id };
    // As in `smp_cpu_offline`: never INIT/SIPI the BSP or an invalid id — an
    // INIT to the BSP resets the machine.
    if lapic_is_bsp_or_invalid(lapic) {
        return -1;
    }

    // A previous wake that got as far as consuming the info block (ACK set) and
    // then failed leaves a core that may still be executing out of that page.
    // Re-stamping it could feed the core a mixed stack/cpu pair, so refuse and
    // leave the core offline: `smp_cpu_offline` clears ACK when it parks a core,
    // so a set ACK here can only mean a failed wake.
    if read_info(INFO_ACK) != 0 {
        return -1;
    }

    // The trampoline page was reserved at boot, so the blob there is still
    // intact — only the info block needs refreshing for this core.  (Do NOT
    // re-copy the blob here: 0x8000 is reserved and holds our own data.)
    write_volatile((TRAMP_ADDR as usize + INFO_ACK) as *mut u32, 0);
    write_volatile((TRAMP_ADDR as usize + INFO_STACK) as *mut u32, stack_top(cpu as usize));
    write_volatile((TRAMP_ADDR as usize + INFO_CPU) as *mut u32, cpu);
    // `ltr` set this core's TSS descriptor busy during the first bring-up and the
    // processor refuses to load a busy TSS, so rewrite it before `smp_ap_entry`
    // runs `ltr` again.
    // SAFETY: `cpu` is bounds-checked at the top of this function, so `p` addresses a live
    // `CPU_TABLE` entry, exclusively owned by the BSP until the AP starts.
    unsafe { install_tss_descriptor(&mut *p, cpu as usize) };

    // SAFETY: the LAPIC id and SIPI vector are the same values the boot-time
    // bring-up used for this core; the helpers do the ICR writes.
    unsafe { ffi::apic_send_init_ipi(lapic) };
    delay_ms(INIT_SIPI_DELAY_MS);
    // SAFETY: as above.
    unsafe { ffi::apic_send_sipi(lapic, TRAMP_VECTOR) };
    delay_ms(SIPI_GAP_MS);
    // SAFETY: as above.
    unsafe { ffi::apic_send_sipi(lapic, TRAMP_VECTOR) };

    // Wait for the core to consume the block, then to finish bring-up.
    let mut ack_waits = 0;
    while read_info(INFO_ACK) != AP_STAGE_TRAMP_READ && ack_waits < 200 {
        delay_ms(10);
        ack_waits += 1;
    }
    if read_info(INFO_ACK) != AP_STAGE_TRAMP_READ {
        // The core never consumed the block, so it is not executing out of the
        // shared page and a retry is safe.
        return -1;
    }

    let mut online_waits = 0;
    while smp_cpu_online(cpu) == 0 && online_waits < 500 {
        delay_ms(10);
        online_waits += 1;
    }
    // Whether it came back or not, the core's state is visible in
    // `/proc/cpuinfo` (online/cstate); a core that consumed the block and then
    // failed is left offline for good, because the ACK it leaves makes further
    // attempts refuse.
    if smp_cpu_online(cpu) == 0 {
        -1
    } else {
        0
    }
}

/// Point `cpu`'s TSS ring-0 stack pointer at `esp0`.  The BSP's TSS is the C
/// `tss_entry` (owned by the scheduler); each AP's TSS lives in `CPU_TABLE`.
/// Called by the scheduler per context switch so ring3→ring0 transitions
/// (interrupts, faults) land on the running task's kernel stack on that core.
pub fn set_tss_esp0(cpu: usize, esp0: u32) {
    if cpu == 0 || cpu >= MAX_CPUS {
        return;
    }
    let p = cpu_ptr(cpu);
    // SAFETY: `cpu` is bounds-checked against `MAX_CPUS` above, so `p` addresses a live
    // `CPU_TABLE` entry and the `tss.esp0` store is in bounds.
    unsafe { (*p).tss.esp0 = esp0 };
}

/// Current CPU index derived from the loaded TSS selector
/// (slot = TSS_SLOT + cpu).
#[no_mangle]
pub extern "C" fn smp_self_cpu() -> i32 {
    let sel: u32;
    // SAFETY: `str` only copies the current task-register selector into an output register; it
    // reads and writes no memory and does not touch the stack.
    unsafe {
        core::arch::asm!("str {0}", out(reg) sel, options(nomem, nostack));
    }
    (((sel as usize) >> 3) as i32) - TSS_SLOT as i32
}
