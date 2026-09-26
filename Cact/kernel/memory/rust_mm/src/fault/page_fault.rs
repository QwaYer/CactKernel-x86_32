//! x86 `#PF` handler logic: COW, demand zero, swap-in, guard `CR3` during PTE walks.

use crate::ffi::*;
use crate::pmm::{kalloc, free_page, page_ref_get_locked, PAGE_LOCK};
use crate::vmm::paging::{get_kernel_pd, vmm_map, PD_KERNEL_ENTRIES};
use crate::fault::swap::{swap_pte_is_swapped, swap_handle_fault};
use crate::fault::oom::oom_kill;
use crate::safe::{KStatic, zero_page, flush_tlb, kprint_str, read_cr2_val, current_page_dir};

/// Switch to the kernel page directory for the rest of this fault handler so PTE
/// walks cannot recurse with user `CR3` when the faulting `pd` omits its own tables.
struct Cr3Guard {
    saved: u32,
    restore: bool,
}

impl Cr3Guard {
    fn install_kernel_pd() -> Self {
        let saved: u32;
        // SAFETY: reading CR3 is side-effect-free.
        unsafe { core::arch::asm!("mov {}, cr3", out(reg) saved, options(nomem, nostack)) };
        let k = get_kernel_pd() as u32;
        // SAFETY: kernel PD is identity-mapped and valid.
        unsafe { core::arch::asm!("mov cr3, {}", in(reg) k, options(nomem, nostack)) };
        Self { saved, restore: true }
    }

    /// Do not restore previous `CR3` on drop (caller will schedule or halt).
    fn dismiss(&mut self) {
        self.restore = false;
    }
}

impl Drop for Cr3Guard {
    fn drop(&mut self) {
        if self.restore {
            // SAFETY: `saved` was read from CR3 at handler entry; valid for restore before iret.
            unsafe { core::arch::asm!("mov cr3, {}", in(reg) self.saved, options(nomem, nostack)) };
        }
    }
}

pub(crate) unsafe fn ensure_private_pt(pd: *mut u32, pdi: usize, extra_flags: u32) -> *mut u32 {
    if pdi >= PD_KERNEL_ENTRIES {
        return core::ptr::null_mut();
    }
    // SAFETY: the caller guarantees `pd` points to a valid page directory and
    // `pdi < PD_KERNEL_ENTRIES`, so this PD entry pointer is in bounds.
    let pde_ptr = unsafe { pd.add(pdi) };
    // SAFETY: `pde_ptr` points at one PD entry, which this call owns (the caller
    // serialises page-table changes); the borrow is consumed by the updates below
    // and no call in between touches this entry.
    let pde = unsafe { &mut *pde_ptr };

    if *pde & PAGE_PRESENT == 0 {
        let pt = kalloc() as *mut u32;
        if pt.is_null() { return core::ptr::null_mut(); }
        zero_page(pt as *mut u8);
        *pde = (pt as u32 & !0xFFF) | PAGE_PRESENT | PAGE_RW | (extra_flags & PAGE_USER) | PDE_PRIVATE;
        return pt;
    }

    if *pde & PDE_PRIVATE == 0 {
        // Shared kernel page table — COW it.
        let shared = (*pde & !0xFFF) as *const u32;
        let priv_pt = kalloc() as *mut u32;
        if priv_pt.is_null() { return core::ptr::null_mut(); }
        // SAFETY: `shared` is a live 1024-entry page table (the PDE is present)
        // and `priv_pt` is a fresh 4 KiB kalloc block, so the copy is in bounds.
        unsafe { core::ptr::copy_nonoverlapping(shared, priv_pt, 1024); }
        let old_flags = *pde & 0xFFF;
        *pde = (priv_pt as u32 & !0xFFF)
            | (old_flags | (extra_flags & PAGE_USER) | PDE_PRIVATE);
        return priv_pt;
    }

    (*pde & !0xFFF) as *mut u32
}

static G_STATS: KStatic<PfStats> = KStatic::new(PfStats {
    total_faults: 0,
    demand_allocs: 0,
    cow_copies: 0,
    stack_grows: 0,
    zero_pages: 0,
    swap_ins: 0,
    protection_faults: 0,
    invalid_access: 0,
});

const PF_PRESENT: u32 = 0x01;
const PF_WRITE: u32 = 0x02;
const PF_USER_BIT: u32 = 0x04;

pub(crate) fn pte_get(pd: *mut u32, vaddr: u32) -> *mut u32 {
    if pd.is_null() {
        return core::ptr::null_mut();
    }
    // SAFETY: `pd` is valid and `pd_index` masks to 10 bits, so this PD entry
    // pointer is in bounds.
    let pde_entry = unsafe { pd.add(pd_index(vaddr) as usize) };
    // SAFETY: `pde_entry` points at one initialised PD entry.
    let pde = unsafe { *pde_entry };
    if pde & PAGE_PRESENT == 0 {
        return core::ptr::null_mut();
    }
    let pt = (pde & !0xFFF) as *mut u32;
    // SAFETY: pt is valid.
    unsafe { pt.add(pt_index(vaddr) as usize) }
}

fn kill_current(fault_addr: u32, _err: u32, _eip: u32, regs: *mut ContextFrame, cr3: &mut Cr3Guard) {
    cr3.dismiss();

    // SAFETY: regs is provided by the interrupt frame.
    unsafe {
        dump_context_frame(regs, fault_addr, SIGSEGV);
    }

    // SAFETY: `current_task` is the C scheduler's global task pointer; reading it
    // from the #PF handler is valid and the pointer, if non-null, stays live
    // until the handler schedules away.
    let t = unsafe { *current_task.get() };
    // SAFETY: `t` is non-null here and points to a live `TaskStruct`.
    if !t.is_null() && unsafe { (*t).is_kernel } == 0 {
        // SAFETY: `(*t).pid` of the live current task.
        let pid = unsafe { (*t).pid };
        // SAFETY: diagnostic counter — `G_STATS` is written only from this #PF handler,
        // which runs with interrupts disabled on this CPU.  Note: not serialised
        // across CPUs; a lost increment is harmless.
        (unsafe { KStatic::get_mut(G_STATS.as_ptr()) }).protection_faults += 1;
        // SAFETY: `task_signal` is a kernel FFI entry point and `pid` comes from
        // the live current task; we are in the #PF handler.
        unsafe { task_signal(pid, SIGSEGV) };
        // SAFETY: `schedule` is a kernel FFI entry point; the faulting task has
        // just been signalled, so yielding here is valid.
        unsafe { schedule() };
        return;
    }

    // SAFETY: diagnostic counter — see the note above: `G_STATS` is only updated
    // from the #PF handler, unsynchronised across CPUs but harmless.
    (unsafe { KStatic::get_mut(G_STATS.as_ptr()) }).protection_faults += 1;
    // SAFETY: `printk_color` takes a valid NUL-terminated string; the literal
    // below is one.
    unsafe {
        printk_color(
            c"[PF] KERNEL PAGE FAULT \u{2014} SYSTEM HALTED\n".as_ptr() as *const u8,
            COLOR_LIGHT_RED,
        );
    }
    // SAFETY: a kernel page fault on the boot path has no recovery; spin in `hlt`
    // forever on this CPU.
    unsafe {
        loop {
            core::arch::asm!("hlt", options(nomem, nostack));
        }
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn page_fault_init() {
    // SAFETY: zeroing the stats struct at boot time, no concurrency.
    unsafe {
        core::ptr::write_bytes(G_STATS.as_ptr() as *mut u8, 0, core::mem::size_of::<PfStats>());
    }
}

/// # Safety
///
/// `regs` must point to the live interrupt frame the CPU pushed for this #PF
/// (a fully valid `ContextFrame`), and the caller must be the IDT vector entry
/// running with interrupts disabled.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn page_fault_handler(regs: *mut ContextFrame) {
    let fault_addr = read_cr2_val();
    // SAFETY: regs is provided by the interrupt frame.
    let err = unsafe { (*regs).err_code };
    // SAFETY: `regs` is the interrupt frame handed to the #PF handler.
    let eip = unsafe { (*regs).eip };

    // SAFETY: `G_STATS.total_faults` bumped from the #PF handler; interrupts are
    // disabled on this CPU, and a cross-CPU lost count is only cosmetic.
    (unsafe { KStatic::get_mut(G_STATS.as_ptr()) }).total_faults += 1;

    // SAFETY: `current_task` global read from the #PF handler.
    let t = unsafe { *current_task.get() };
    // SAFETY: `t` is non-null and live; `page_directory` is a valid pointer or null.
    let pd = if !t.is_null() && !unsafe { (*t).page_directory }.is_null() {
        // SAFETY: same live `TaskStruct`; the non-null check is the enclosing `if`.
        unsafe { (*t).page_directory }
    } else {
        current_page_dir()
    };

    let mut cr3_guard = Cr3Guard::install_kernel_pd();

    if (err & PF_PRESENT != 0) && (err & PF_WRITE != 0) {
        let pte_ptr = pte_get(pd, fault_addr);

        // COW handler: present + write + PAGE_COW in PTE.
        // SAFETY: `pte_ptr` is the result of `pte_get`, checked non-null on the left of
        // the `&&`; it points into the faulting address space's page table.
        if !pte_ptr.is_null() && unsafe { *pte_ptr & PAGE_COW != 0 }
            && vmm_handle_cow(pd, fault_addr & !0xFFF) == 0 {
                // SAFETY: `G_STATS.cow_copies` — unprotected diagnostic counter, see the note
                // at the top of the handler.
                (unsafe { KStatic::get_mut(G_STATS.as_ptr()) }).cow_copies += 1;
                return;
            }

        // Protection fault from user mode: page is present but lacks PAGE_USER.
        // This happens when the stack grows into a page that was inherited from
        // the kernel identity map (PAGE_PRESENT|PAGE_RW, no PAGE_USER).
        // Allocate a fresh zero user page and remap it properly.
        if (err & PF_USER_BIT != 0)
            && (USER_STACK_LIMIT..USER_STACK_TOP).contains(&fault_addr)
            && !pte_ptr.is_null()
            // SAFETY: `pte_ptr` is non-null (checked in the enclosing condition) and points
            // at the PTE for this fault address.
            && unsafe { *pte_ptr & PAGE_USER == 0 }
        {
            let page_va = fault_addr & !0xFFF;
            let mut phys = kalloc();
            if phys.is_null() && oom_kill() == 0 {
                phys = kalloc();
            }
            if phys.is_null() {
                kill_current(fault_addr, err, eip, regs, &mut cr3_guard);
                return;
            }
            zero_page(phys);
            // SAFETY: `pd` is the faulting task's page directory, `page_va` is in
            // the user stack window and `phys` is the zeroed frame just allocated.
            unsafe { vmm_map(pd, page_va, phys as u32, (PAGE_PRESENT | PAGE_RW | PAGE_USER) as i32); }
            flush_tlb(page_va);
            // SAFETY: `G_STATS.stack_grows` — unprotected diagnostic counter; see the
            // note above.
            (unsafe { KStatic::get_mut(G_STATS.as_ptr()) }).stack_grows += 1;
            return;
        }

        kill_current(fault_addr, err, eip, regs, &mut cr3_guard);
        return;
    }

    // Read protection fault from user mode (PF_PRESENT=1, no PF_WRITE) on a
    // page in the stack region that has no PAGE_USER.  Same root cause as the
    // write case above: kernel identity-map entry without PAGE_USER.
    if (err & PF_PRESENT != 0)
        && (err & PF_USER_BIT != 0)
        && (USER_STACK_LIMIT..USER_STACK_TOP).contains(&fault_addr)
    {
        let pte_ptr = pte_get(pd, fault_addr);
        // SAFETY: `pte_ptr` is non-null on the left of `&&` and valid for this fault.
        if !pte_ptr.is_null() && unsafe { *pte_ptr & PAGE_USER == 0 } {
            let page_va = fault_addr & !0xFFF;
            let mut phys = kalloc();
            if phys.is_null() && oom_kill() == 0 {
                phys = kalloc();
            }
            if phys.is_null() {
                kill_current(fault_addr, err, eip, regs, &mut cr3_guard);
                return;
            }
            zero_page(phys);
            // SAFETY: `pd` is the faulting task's page directory, `page_va` is in
            // the user stack window and `phys` is the zeroed frame just allocated.
            unsafe { vmm_map(pd, page_va, phys as u32, (PAGE_PRESENT | PAGE_RW | PAGE_USER) as i32); }
            flush_tlb(page_va);
            // SAFETY: `G_STATS.stack_grows` — unprotected diagnostic counter; see above.
            (unsafe { KStatic::get_mut(G_STATS.as_ptr()) }).stack_grows += 1;
            return;
        }
    }

    if err & PF_PRESENT == 0 {
        let page_va = fault_addr & !0xFFF;
        let pte = pte_get(pd, fault_addr);

        // SAFETY: `pte` is non-null on the left of `&&`; it is the PTE for the faulting
        // address in `pd`.
        if !pte.is_null() && swap_pte_is_swapped(unsafe { *pte }) {
            // SAFETY: `pd` is the faulting task's page directory, `fault_addr` is
            // the address that just faulted, and a swapped PTE was confirmed above.
            if unsafe { swap_handle_fault(pd, fault_addr) } == 0 {
                // SAFETY: `G_STATS.swap_ins` — unprotected diagnostic counter; see above.
                (unsafe { KStatic::get_mut(G_STATS.as_ptr()) }).swap_ins += 1;
                return;
            }
            kill_current(fault_addr, err, eip, regs, &mut cr3_guard);
            return;
        }

        // SAFETY: `pte` is non-null on the left of `&&`; the demand bit lives in that
        // live PTE.
        if !pte.is_null() && unsafe { *pte & PAGE_DEMAND != 0 } {
            let mut phys = kalloc();
            if phys.is_null() && oom_kill() == 0 {
                phys = kalloc();
            }
            if phys.is_null() {
                kill_current(fault_addr, err, eip, regs, &mut cr3_guard);
                return;
            }

            // SAFETY: `pte` is non-null (checked above) and its demand flag is set.
            if unsafe { *pte & PAGE_ZERO != 0 } {
                zero_page(phys);
                // SAFETY: `G_STATS.zero_pages` — unprotected diagnostic counter; see above.
                (unsafe { KStatic::get_mut(G_STATS.as_ptr()) }).zero_pages += 1;
            }

            // SAFETY: pte is valid.
            let pte_val = unsafe { *pte };
            let flags = (pte_val & 0xFFF) & !(PAGE_DEMAND | PAGE_ZERO);
            let flags = flags | PAGE_PRESENT | PAGE_RW;
            // SAFETY: `pte` is the live PTE for this demand page; the new frame `phys` was
            // just allocated and zeroed, so publishing it here is well-defined.
            unsafe { *pte = (phys as u32 & !0xFFF) | flags; }
            flush_tlb(page_va);

            // SAFETY: `G_STATS.demand_allocs` — unprotected diagnostic counter; see above.
            (unsafe { KStatic::get_mut(G_STATS.as_ptr()) }).demand_allocs += 1;
            return;
        }

        // SAFETY: `pte` non-null on the left of `&&`, COW bit set in that live PTE.
        if !pte.is_null() && unsafe { *pte & PAGE_COW != 0 } {
            // SAFETY: `pte` is non-null and present; the low bits masked off give the
            // page-aligned physical frame it points at.
            let old_phys = unsafe { *pte & !0xFFF } as *mut u8;

            // ── Sole-owner check + act under PAGE_LOCK (C-01 fix) ────────────
            crate::safe::lock_acquire(PAGE_LOCK.as_ptr());
            let rc = page_ref_get_locked(old_phys);
            if rc <= 1 {
                // Sole owner — promote the existing frame to writable in-place.
                // SAFETY: `pte` is a live PTE (non-null, checked above).
                let pte_val = unsafe { *pte };
                let flags = ((pte_val & 0xFFF) & !PAGE_COW) | PAGE_PRESENT | PAGE_RW;
                // SAFETY: promoting the existing frame to writable under `PAGE_LOCK`; `pte` is
                // the valid PTE for this page.
                unsafe { *pte = (old_phys as u32 & !0xFFF) | flags; }
                crate::safe::lock_release(PAGE_LOCK.as_ptr());
                flush_tlb(page_va);
                // SAFETY: `G_STATS.cow_copies` — unprotected diagnostic counter; see above.
                (unsafe { KStatic::get_mut(G_STATS.as_ptr()) }).cow_copies += 1;
                return;
            }
            crate::safe::lock_release(PAGE_LOCK.as_ptr());

            // ── Multi-owner — allocate a private copy ────────────────────────
            let mut phys = kalloc();
            if phys.is_null() && oom_kill() == 0 {
                phys = kalloc();
            }
            if phys.is_null() {
                kill_current(fault_addr, err, eip, regs, &mut cr3_guard);
                return;
            }

            // SAFETY: copying page contents.
            unsafe {
                core::ptr::copy_nonoverlapping(old_phys as *const u8, phys, PAGE_SIZE as usize);
            }
            // SAFETY: `pte` is non-null and present; its value is read to derive the new
            // PTE flags.
            let pte_val = unsafe { *pte };
            let flags = ((pte_val & 0xFFF) & !PAGE_COW) | PAGE_PRESENT | PAGE_RW;
            // SAFETY: publishing the freshly copied frame `phys` into the live PTE.
            unsafe { *pte = (phys as u32 & !0xFFF) | flags; }
            // SAFETY: the frame is live and PMM-managed; this call releases exactly one reference to it.
            unsafe { free_page(old_phys) };
            flush_tlb(page_va);

            // SAFETY: `G_STATS.cow_copies` — unprotected diagnostic counter; see above.
            (unsafe { KStatic::get_mut(G_STATS.as_ptr()) }).cow_copies += 1;
            return;
        }

        if (USER_STACK_LIMIT..USER_STACK_TOP).contains(&fault_addr) {
            // Stack grow: missing page OR present kernel-only page (no PAGE_USER).
            let existing = pte_get(pd, fault_addr);
            let need_alloc = existing.is_null()
                // SAFETY: `existing` is the PTE returned by `pte_get` and checked non-null on
                // the left of `||`.
                || unsafe { *existing & PAGE_PRESENT == 0 }
                // SAFETY: same non-null `existing` PTE as the previous condition.
                || unsafe { *existing & PAGE_USER == 0 };

            if need_alloc {
                let mut phys = kalloc();
                if phys.is_null() && oom_kill() == 0 {
                    phys = kalloc();
                }
                if phys.is_null() {
                    kill_current(fault_addr, err, eip, regs, &mut cr3_guard);
                    return;
                }
                zero_page(phys);
                // SAFETY: `pd` is valid, `page_va` is inside the user stack window
                // and `phys` is the zeroed frame just allocated for it.
                unsafe {
                    vmm_map(
                        pd,
                        page_va,
                        phys as u32,
                        (PAGE_PRESENT | PAGE_RW | PAGE_USER) as i32,
                    );
                }
                flush_tlb(page_va);
                // SAFETY: `G_STATS.stack_grows` — unprotected diagnostic counter; see above.
                (unsafe { KStatic::get_mut(G_STATS.as_ptr()) }).stack_grows += 1;
                return;
            }
        }

        // SAFETY: `G_STATS.invalid_access` — unprotected diagnostic counter; see above.
        (unsafe { KStatic::get_mut(G_STATS.as_ptr()) }).invalid_access += 1;
        kill_current(fault_addr, err, eip, regs, &mut cr3_guard);
        return;
    }
    kill_current(fault_addr, err, eip, regs, &mut cr3_guard);
}

#[path = "page_fault_map.rs"]
mod page_fault_map;
pub use page_fault_map::*;

#[unsafe(no_mangle)]
pub extern "C" fn pf_get_stats() -> PfStats {
    // SAFETY: read-only snapshot of the #PF counters for the C caller.
    *unsafe { KStatic::get_mut(G_STATS.as_ptr()) }
}

#[unsafe(no_mangle)]
pub extern "C" fn pf_print_stats() {
    let mut buf = [0u8; 16];
    kprint_str(c"[PF] === Page Fault Statistics ===\n".as_ptr() as *const u8);

    macro_rules! pf_stat {
        ($label:expr, $field:expr) => {
            kprint_str($label.as_ptr());
            // SAFETY: `buf` is a live 16-byte stack array and `itoa` writes at most its
            // digits plus terminator; `printk` then reads that NUL-terminated buffer.
            unsafe {
                itoa($field as i32, buf.as_mut_ptr());
                printk(buf.as_ptr());
            }
            kprint_str(b"\n\0".as_ptr());
        };
    }

    // SAFETY: read-only snapshot of `G_STATS` for printing.
    let stats = unsafe { KStatic::get_mut(G_STATS.as_ptr()) };
    pf_stat!(b"  total_faults: \0", stats.total_faults);
    pf_stat!(b"  demand_allocs: \0", stats.demand_allocs);
    pf_stat!(b"  cow_copies: \0", stats.cow_copies);
    pf_stat!(b"  stack_grows: \0", stats.stack_grows);
    pf_stat!(b"  zero_pages: \0", stats.zero_pages);
    pf_stat!(b"  swap_ins: \0", stats.swap_ins);
    pf_stat!(b"  prot_faults: \0", stats.protection_faults);
    pf_stat!(b"  invalid_access: \0", stats.invalid_access);
}
