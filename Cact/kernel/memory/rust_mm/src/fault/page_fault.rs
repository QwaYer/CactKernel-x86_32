use crate::ffi::*;
use crate::pmm::{kalloc, kfree_page, page_ref_get};
use crate::vmm::paging::vmm_map;
use crate::fault::swap::{swap_pte_is_swapped, swap_handle_fault};
use crate::fault::oom::oom_kill;
use crate::safe::{KStatic, zero_page, flush_tlb, kprint_str, read_cr2_val, current_page_dir};

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

fn pte_get(pd: *mut u32, vaddr: u32) -> *mut u32 {
    if pd.is_null() {
        return core::ptr::null_mut();
    }
    // SAFETY: pd is valid and we bounds-check via pd_index/pt_index.
    let pde = unsafe { *pd.add(pd_index(vaddr) as usize) };
    if pde & PAGE_PRESENT == 0 {
        return core::ptr::null_mut();
    }
    let pt = (pde & !0xFFF) as *mut u32;
    // SAFETY: pt is valid.
    unsafe { pt.add(pt_index(vaddr) as usize) }
}

fn kill_current(fault_addr: u32, err: u32, eip: u32) {
    let t = unsafe { current_task };

    // SAFETY: kprint_color is a C function that takes valid strings.
    unsafe { kprint_color(b"\n[PF] SEGFAULT pid=\0".as_ptr(), COLOR_LIGHT_RED); }
    let mut buf = [0u8; 12];
    let pid = if !t.is_null() { unsafe { (*t).pid as i32 } } else { -1 };
    unsafe {
        itoa(pid, buf.as_mut_ptr());
        kprint_color(buf.as_ptr(), COLOR_LIGHT_RED);
    }

    unsafe {
        kprint_color(b" addr=0x\0".as_ptr(), COLOR_LIGHT_RED);
        kprint_hex(fault_addr);
        kprint_color(b" err=0x\0".as_ptr(), COLOR_LIGHT_RED);
        kprint_hex(err);
        kprint_color(b" eip=0x\0".as_ptr(), COLOR_LIGHT_RED);
        kprint_hex(eip);
    }
    kprint_str(b"\n\0".as_ptr());

    G_STATS.get_mut().protection_faults += 1;

    if !t.is_null() && unsafe { (*t).is_kernel } == 0 {
        let pid = unsafe { (*t).pid };
        // SAFETY: task_signal and schedule are kernel FFI entry points.
        unsafe {
            task_signal(pid, SIGSEGV);
            schedule();
        }
        return;
    }

    // SAFETY: kernel page fault — system must halt.
    unsafe {
        kprint_color(
            b"[PF] KERNEL PAGE FAULT \xe2\x80\x94 SYSTEM HALTED\n\0".as_ptr(),
            COLOR_LIGHT_RED,
        );
        loop {
            core::arch::asm!("hlt", options(nomem, nostack));
        }
    }
}

//public api
#[unsafe(no_mangle)]
pub extern "C" fn page_fault_init() {
    kprint_str(b"[PF] clearing stats  handlers: COW demand-paging stack-grow swap-in SEGFAULT\n\0".as_ptr());
    // SAFETY: zeroing the stats struct at boot time, no concurrency.
    unsafe {
        core::ptr::write_bytes(G_STATS.as_ptr() as *mut u8, 0, core::mem::size_of::<PfStats>());
    }
    crate::safe::klog_msg(LOG_OK, b"page fault handler ready\0".as_ptr());
}

//public api
#[unsafe(no_mangle)]
pub extern "C" fn page_fault_handler(regs: *mut ContextFrame) {
    let fault_addr = read_cr2_val();
    // SAFETY: regs is provided by the interrupt frame.
    let err = unsafe { (*regs).err_code };
    let eip = unsafe { (*regs).eip };

    G_STATS.get_mut().total_faults += 1;

    let t = unsafe { current_task };
    let pd = if !t.is_null() && !unsafe { (*t).page_directory }.is_null() {
        unsafe { (*t).page_directory }
    } else {
        current_page_dir()
    };

    if (err & PF_PRESENT != 0) && (err & PF_WRITE != 0) {
        let pte = pte_get(pd, fault_addr);
        if !pte.is_null() && unsafe { *pte & PAGE_COW != 0 } {
            if vmm_handle_cow(pd, fault_addr & !0xFFF) == 0 {
                G_STATS.get_mut().cow_copies += 1;
                return;
            }
        }
        kill_current(fault_addr, err, eip);
        return;
    }

    if err & PF_PRESENT == 0 {
        let page_va = fault_addr & !0xFFF;
        let pte = pte_get(pd, fault_addr);

        if !pte.is_null() && swap_pte_is_swapped(unsafe { *pte }) {
            if swap_handle_fault(pd, fault_addr) == 0 {
                G_STATS.get_mut().swap_ins += 1;
                return;
            }
            kill_current(fault_addr, err, eip);
            return;
        }

        if !pte.is_null() && unsafe { *pte & PAGE_DEMAND != 0 } {
            let mut phys = kalloc();
            if phys.is_null() && oom_kill() == 0 {
                phys = kalloc();
            }
            if phys.is_null() {
                kill_current(fault_addr, err, eip);
                return;
            }

            if unsafe { *pte & PAGE_ZERO != 0 } {
                zero_page(phys);
                G_STATS.get_mut().zero_pages += 1;
            }

            // SAFETY: pte is valid.
            let pte_val = unsafe { *pte };
            let flags = (pte_val & 0xFFF) & !(PAGE_DEMAND | PAGE_ZERO);
            let flags = flags | PAGE_PRESENT | PAGE_RW;
            unsafe { *pte = (phys as u32 & !0xFFF) | flags; }
            flush_tlb(page_va);

            G_STATS.get_mut().demand_allocs += 1;
            return;
        }

        if !pte.is_null() && unsafe { *pte & PAGE_COW != 0 } {
            let old_phys = unsafe { *pte & !0xFFF } as *mut u8;
            let mut phys = kalloc();
            if phys.is_null() && oom_kill() == 0 {
                phys = kalloc();
            }
            if phys.is_null() {
                kill_current(fault_addr, err, eip);
                return;
            }

            if page_ref_get(old_phys) <= 1 {
                // Sole owner — just make writable, no copy needed
                let pte_val = unsafe { *pte };
                let flags = (pte_val & 0xFFF) & !PAGE_COW;
                let flags = flags | PAGE_PRESENT | PAGE_RW;
                unsafe { *pte = (old_phys as u32 & !0xFFF) | flags; }
                kfree_page(phys);
            } else {
                // Multiple refs — copy old content
                // SAFETY: copying page contents.
                unsafe {
                    core::ptr::copy_nonoverlapping(old_phys as *const u8, phys, PAGE_SIZE as usize);
                }
                let pte_val = unsafe { *pte };
                let flags = (pte_val & 0xFFF) & !PAGE_COW;
                let flags = flags | PAGE_PRESENT | PAGE_RW;
                unsafe { *pte = (phys as u32 & !0xFFF) | flags; }
                kfree_page(old_phys);
            }
            flush_tlb(page_va);

            G_STATS.get_mut().cow_copies += 1;
            return;
        }

        if fault_addr >= USER_STACK_LIMIT && fault_addr < USER_STACK_TOP {
            let mut phys = kalloc();
            if phys.is_null() && oom_kill() == 0 {
                phys = kalloc();
            }
            if phys.is_null() {
                kill_current(fault_addr, err, eip);
                return;
            }
            zero_page(phys);
            vmm_map(
                pd,
                page_va,
                phys as u32,
                (PAGE_PRESENT | PAGE_RW | PAGE_USER) as i32,
            );
            flush_tlb(page_va);

            G_STATS.get_mut().stack_grows += 1;
            return;
        }

        if fault_addr < 0xC0000000 && (err & PF_USER_BIT != 0) {
            // SAFETY: pd is valid.
            let pdi = pd_index(fault_addr) as usize;
            let pde = unsafe { *pd.add(pdi) };
            if pde & PAGE_PRESENT == 0 {
                kprint_str(b"[PF] DBG: no PDE for addr=0x\0".as_ptr());
                unsafe { kprint_hex(fault_addr); }
                kprint_str(b"\n\0".as_ptr());
            } else if pte.is_null() {
                kprint_str(b"[PF] DBG: pte_get returned null for addr=0x\0".as_ptr());
                unsafe { kprint_hex(fault_addr); }
                kprint_str(b"\n\0".as_ptr());
            } else {
                kprint_str(b"[PF] DBG: PTE=0x\0".as_ptr());
                unsafe { kprint_hex(*pte); }
                kprint_str(b" addr=0x\0".as_ptr());
                unsafe { kprint_hex(fault_addr); }
                kprint_str(b"\n\0".as_ptr());
            }
        }

        G_STATS.get_mut().invalid_access += 1;
        kill_current(fault_addr, err, eip);
        return;
    }
    kill_current(fault_addr, err, eip);
}

//public api
#[unsafe(no_mangle)]
pub extern "C" fn vmm_map_demand(
    pd: *mut u32,
    virtual_addr: u32,
    size: u32,
    flags: i32,
) -> i32 {
    if pd.is_null() || size == 0 {
        return -1;
    }
    let flags = flags as u32;
    let start = virtual_addr & !0xFFF;
    let end = (virtual_addr + size + 0xFFF) & !0xFFF;

    let mut va = start;
    while va < end {
        let pdi = pd_index(va) as usize;

        // SAFETY: pd is valid.
        unsafe {
            if *pd.add(pdi) & PAGE_PRESENT == 0 {
                let pt = kalloc() as *mut u32;
                if pt.is_null() {
                    return -1;
                }
                zero_page(pt as *mut u8);
                *pd.add(pdi) = (pt as u32 & !0xFFF) | PAGE_PRESENT | PAGE_RW | (flags & PAGE_USER);
            }

            let pt = (*pd.add(pdi) & !0xFFF) as *mut u32;
            *pt.add(pt_index(va) as usize) = PAGE_DEMAND | (flags & PAGE_USER);
        }
        va += PAGE_SIZE;
    }
    0
}

//public api
#[unsafe(no_mangle)]
pub extern "C" fn vmm_map_zero(
    pd: *mut u32,
    virtual_addr: u32,
    size: u32,
    flags: i32,
) -> i32 {
    if pd.is_null() || size == 0 {
        return -1;
    }
    let flags = flags as u32;
    let start = virtual_addr & !0xFFF;
    let end = (virtual_addr + size + 0xFFF) & !0xFFF;

    let mut va = start;
    while va < end {
        let pdi = pd_index(va) as usize;

        // SAFETY: pd is valid.
        unsafe {
            if *pd.add(pdi) & PAGE_PRESENT == 0 {
                let pt = kalloc() as *mut u32;
                if pt.is_null() {
                    return -1;
                }
                zero_page(pt as *mut u8);
                *pd.add(pdi) = (pt as u32 & !0xFFF) | PAGE_PRESENT | PAGE_RW | (flags & PAGE_USER);
            }

            let pt = (*pd.add(pdi) & !0xFFF) as *mut u32;
            *pt.add(pt_index(va) as usize) = PAGE_DEMAND | PAGE_ZERO | (flags & PAGE_USER);
        }
        va += PAGE_SIZE;
    }
    0
}

//public api
#[unsafe(no_mangle)]
pub extern "C" fn vmm_map_cow(pd: *mut u32, virtual_addr: u32) -> i32 {
    let pte = pte_get(pd, virtual_addr);
    if pte.is_null() || unsafe { *pte & PAGE_PRESENT == 0 } {
        return -1;
    }
    // SAFETY: pte is valid.
    unsafe { *pte = (*pte & !PAGE_RW) | PAGE_COW; }
    flush_tlb(virtual_addr & !0xFFF);
    0
}

//public api
#[unsafe(no_mangle)]
pub extern "C" fn vmm_is_cow_page(pd: *mut u32, virtual_addr: u32) -> i32 {
    let pte = pte_get(pd, virtual_addr);
    if !pte.is_null() && unsafe { *pte & PAGE_COW != 0 } {
        1
    } else {
        0
    }
}

//public api
#[unsafe(no_mangle)]
pub extern "C" fn vmm_handle_cow(pd: *mut u32, virtual_addr: u32) -> i32 {
    let pte = pte_get(pd, virtual_addr);
    if pte.is_null() || unsafe { *pte & PAGE_COW == 0 } {
        return -1;
    }

    // SAFETY: pte is valid and has COW flag.
    let old_phys = unsafe { *pte & !0xFFF } as *mut u8;
    let pte_val = unsafe { *pte };
    let flags = (pte_val & 0xFFF) & !PAGE_COW;
    let flags = flags | PAGE_RW | PAGE_PRESENT;

    // Sole owner — just make writable again, no copy needed
    if page_ref_get(old_phys) == 1 {
        unsafe { *pte = (old_phys as u32 & !0xFFF) | flags; }
        flush_tlb(virtual_addr & !0xFFF);
        return 0;
    }

    // Multiple references — allocate a private copy
    let new_phys = kalloc();
    if new_phys.is_null() {
        return -1;
    }

    // SAFETY: copying page contents.
    unsafe {
        core::ptr::copy_nonoverlapping(old_phys as *const u8, new_phys, PAGE_SIZE as usize);
        *pte = (new_phys as u32 & !0xFFF) | flags;
    }

    // Release our reference to the shared page
    kfree_page(old_phys);

    flush_tlb(virtual_addr & !0xFFF);
    0
}

//public api
#[unsafe(no_mangle)]
pub extern "C" fn vmm_setup_user_stack(pd: *mut u32, initial_size: u32) -> u32 {
    if pd.is_null() {
        return 0;
    }

    let mut initial_size = (initial_size + 0xFFF) & !0xFFF;
    if initial_size == 0 {
        initial_size = PAGE_SIZE;
    }

    let bottom = USER_STACK_TOP - initial_size;

    let mut va = bottom;
    while va < USER_STACK_TOP {
        let phys = kalloc();
        if phys.is_null() {
            return 0;
        }
        zero_page(phys);
        vmm_map(
            pd,
            va,
            phys as u32,
            (PAGE_PRESENT | PAGE_RW | PAGE_USER) as i32,
        );
        va += PAGE_SIZE;
    }

    USER_STACK_TOP
}

//public api
#[unsafe(no_mangle)]
pub extern "C" fn pf_get_stats() -> PfStats {
    *G_STATS.get_mut()
}

//public api
#[unsafe(no_mangle)]
pub extern "C" fn pf_print_stats() {
    let mut buf = [0u8; 16];
    kprint_str(b"[PF] === Page Fault Statistics ===\n\0".as_ptr());

    macro_rules! pf_stat {
        ($label:expr, $field:expr) => {
            kprint_str($label.as_ptr());
            unsafe {
                itoa($field as i32, buf.as_mut_ptr());
                kprint(buf.as_ptr());
            }
            kprint_str(b"\n\0".as_ptr());
        };
    }

    let stats = G_STATS.get_mut();
    pf_stat!(b"  total_faults: \0", stats.total_faults);
    pf_stat!(b"  demand_allocs: \0", stats.demand_allocs);
    pf_stat!(b"  cow_copies: \0", stats.cow_copies);
    pf_stat!(b"  stack_grows: \0", stats.stack_grows);
    pf_stat!(b"  zero_pages: \0", stats.zero_pages);
    pf_stat!(b"  swap_ins: \0", stats.swap_ins);
    pf_stat!(b"  prot_faults: \0", stats.protection_faults);
    pf_stat!(b"  invalid_access: \0", stats.invalid_access);
}
