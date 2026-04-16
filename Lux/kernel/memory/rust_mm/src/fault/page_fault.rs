use crate::ffi::*;
use crate::pmm::{kalloc, kfree_page, page_ref_get};
use crate::vmm::paging::vmm_map;
use crate::fault::swap::{swap_pte_is_swapped, swap_handle_fault};
use crate::fault::oom::oom_kill;

static mut G_STATS: PfStats = PfStats {
    total_faults: 0,
    demand_allocs: 0,
    cow_copies: 0,
    stack_grows: 0,
    zero_pages: 0,
    swap_ins: 0,
    protection_faults: 0,
    invalid_access: 0,
};

const PF_PRESENT: u32 = 0x01;
const PF_WRITE: u32 = 0x02;
const PF_USER_BIT: u32 = 0x04;

unsafe fn pte_get(pd: *mut u32, vaddr: u32) -> *mut u32 {
    if pd.is_null() {
        return core::ptr::null_mut();
    }
    let pde = *pd.add(pd_index(vaddr) as usize);
    if pde & PAGE_PRESENT == 0 {
        return core::ptr::null_mut();
    }
    let pt = (pde & !0xFFF) as *mut u32;
    pt.add(pt_index(vaddr) as usize)
}

unsafe fn zero_page(phys: *mut u8) {
    core::ptr::write_bytes(phys, 0, PAGE_SIZE as usize);
}

unsafe fn kill_current(fault_addr: u32, err: u32, eip: u32) {
    let t = current_task;

    kprint_color(b"\n[PF] SEGFAULT pid=\0".as_ptr(), COLOR_LIGHT_RED);
    let mut buf = [0u8; 12];
    let pid = if !t.is_null() { (*t).pid as i32 } else { -1 };
    itoa(pid, buf.as_mut_ptr());
    kprint_color(buf.as_ptr(), COLOR_LIGHT_RED);

    kprint_color(b" addr=0x\0".as_ptr(), COLOR_LIGHT_RED);
    kprint_hex(fault_addr);
    kprint_color(b" err=0x\0".as_ptr(), COLOR_LIGHT_RED);
    kprint_hex(err);
    kprint_color(b" eip=0x\0".as_ptr(), COLOR_LIGHT_RED);
    kprint_hex(eip);
    kprint(b"\n\0".as_ptr());

    G_STATS.protection_faults += 1;

    if !t.is_null() && (*t).is_kernel == 0 {
        task_signal((*t).pid, SIGSEGV);
        schedule();
        return;
    }

    kprint_color(
        b"[PF] KERNEL PAGE FAULT \xe2\x80\x94 SYSTEM HALTED\n\0".as_ptr(),
        COLOR_LIGHT_RED,
    );
    loop {
        core::arch::asm!("hlt", options(nomem, nostack));
    }
}

//public api
#[unsafe(no_mangle)]
pub unsafe extern "C" fn page_fault_init() {
    kprint(b"[PF] clearing stats  handlers: COW demand-paging stack-grow swap-in SEGFAULT\n\0".as_ptr());
    core::ptr::write_bytes(&raw mut G_STATS as *mut u8, 0, core::mem::size_of::<PfStats>());
    klog(LOG_OK, b"page fault handler ready\0".as_ptr());
}

//public api
#[unsafe(no_mangle)]
pub unsafe extern "C" fn page_fault_handler(regs: *mut ContextFrame) {
    let fault_addr = read_cr2();
    let err = (*regs).err_code;
    let eip = (*regs).eip;

    G_STATS.total_faults += 1;

    let t = current_task;
    let pd = if !t.is_null() && !(*t).page_directory.is_null() {
        (*t).page_directory
    } else {
        get_current_pd()
    };

    if (err & PF_PRESENT != 0) && (err & PF_WRITE != 0) {
        let pte = pte_get(pd, fault_addr);
        if !pte.is_null() && (*pte & PAGE_COW != 0) {
            if vmm_handle_cow(pd, fault_addr & !0xFFF) == 0 {
                G_STATS.cow_copies += 1;
                return;
            }
        }
        kill_current(fault_addr, err, eip);
        return;
    }

    if err & PF_PRESENT == 0 {
        let page_va = fault_addr & !0xFFF;
        let pte = pte_get(pd, fault_addr);

        if !pte.is_null() && swap_pte_is_swapped(*pte) {
            if swap_handle_fault(pd, fault_addr) == 0 {
                G_STATS.swap_ins += 1;
                return;
            }
            kill_current(fault_addr, err, eip);
            return;
        }

        if !pte.is_null() && (*pte & PAGE_DEMAND != 0) {
            let mut phys = kalloc();
            if phys.is_null() && oom_kill() == 0 {
                phys = kalloc();
            }
            if phys.is_null() {
                kill_current(fault_addr, err, eip);
                return;
            }

            if *pte & PAGE_ZERO != 0 {
                zero_page(phys);
                G_STATS.zero_pages += 1;
            }

            let flags = (*pte & 0xFFF) & !(PAGE_DEMAND | PAGE_ZERO);
            let flags = flags | PAGE_PRESENT | PAGE_RW;
            *pte = (phys as u32 & !0xFFF) | flags;
            tlb_flush(page_va);

            G_STATS.demand_allocs += 1;
            return;
        }

        if !pte.is_null() && (*pte & PAGE_COW != 0) {
            let old_phys = (*pte & !0xFFF) as *mut u8;
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
                let flags = (*pte & 0xFFF) & !PAGE_COW;
                let flags = flags | PAGE_PRESENT | PAGE_RW;
                *pte = (old_phys as u32 & !0xFFF) | flags;
                kfree_page(phys);
            } else {
                // Multiple refs — copy old content
                core::ptr::copy_nonoverlapping(old_phys as *const u8, phys, PAGE_SIZE as usize);
                let flags = (*pte & 0xFFF) & !PAGE_COW;
                let flags = flags | PAGE_PRESENT | PAGE_RW;
                *pte = (phys as u32 & !0xFFF) | flags;
                kfree_page(old_phys as *mut u8);
            }
            tlb_flush(page_va);

            G_STATS.cow_copies += 1;
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
            tlb_flush(page_va);

            G_STATS.stack_grows += 1;
            return;
        }

        if fault_addr < 0xC0000000 && (err & PF_USER_BIT != 0) {
            let pdi = pd_index(fault_addr) as usize;
            if *pd.add(pdi) & PAGE_PRESENT == 0 {
                kprint(b"[PF] DBG: no PDE for addr=0x\0".as_ptr());
                kprint_hex(fault_addr);
                kprint(b"\n\0".as_ptr());
            } else if pte.is_null() {
                kprint(b"[PF] DBG: pte_get returned null for addr=0x\0".as_ptr());
                kprint_hex(fault_addr);
                kprint(b"\n\0".as_ptr());
            } else {
                kprint(b"[PF] DBG: PTE=0x\0".as_ptr());
                kprint_hex(*pte);
                kprint(b" addr=0x\0".as_ptr());
                kprint_hex(fault_addr);
                kprint(b"\n\0".as_ptr());
            }
        }

        G_STATS.invalid_access += 1;
        kill_current(fault_addr, err, eip);
        return;
    }
    kill_current(fault_addr, err, eip);
}

//public api
#[unsafe(no_mangle)]
pub unsafe extern "C" fn vmm_map_demand(
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
        va += PAGE_SIZE;
    }
    0
}

//public api
#[unsafe(no_mangle)]
pub unsafe extern "C" fn vmm_map_zero(
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
        va += PAGE_SIZE;
    }
    0
}

//public api
#[unsafe(no_mangle)]
pub unsafe extern "C" fn vmm_map_cow(pd: *mut u32, virtual_addr: u32) -> i32 {
    let pte = pte_get(pd, virtual_addr);
    if pte.is_null() || (*pte & PAGE_PRESENT == 0) {
        return -1;
    }
    *pte = (*pte & !PAGE_RW) | PAGE_COW;
    tlb_flush(virtual_addr & !0xFFF);
    0
}

//public api
#[unsafe(no_mangle)]
pub unsafe extern "C" fn vmm_is_cow_page(pd: *mut u32, virtual_addr: u32) -> i32 {
    let pte = pte_get(pd, virtual_addr);
    if !pte.is_null() && (*pte & PAGE_COW != 0) {
        1
    } else {
        0
    }
}

//public api
#[unsafe(no_mangle)]
pub unsafe extern "C" fn vmm_handle_cow(pd: *mut u32, virtual_addr: u32) -> i32 {
    let pte = pte_get(pd, virtual_addr);
    if pte.is_null() || (*pte & PAGE_COW == 0) {
        return -1;
    }

    let old_phys = (*pte & !0xFFF) as *mut u8;
    let flags = (*pte & 0xFFF) & !PAGE_COW;
    let flags = flags | PAGE_RW | PAGE_PRESENT;

    // Sole owner — just make writable again, no copy needed
    if page_ref_get(old_phys) == 1 {
        *pte = (old_phys as u32 & !0xFFF) | flags;
        tlb_flush(virtual_addr & !0xFFF);
        return 0;
    }

    // Multiple references — allocate a private copy
    let new_phys = kalloc();
    if new_phys.is_null() {
        return -1;
    }

    core::ptr::copy_nonoverlapping(old_phys as *const u8, new_phys, PAGE_SIZE as usize);

    *pte = (new_phys as u32 & !0xFFF) | flags;

    // Release our reference to the shared page
    kfree_page(old_phys);

    tlb_flush(virtual_addr & !0xFFF);
    0
}

//public api
#[unsafe(no_mangle)]
pub unsafe extern "C" fn vmm_setup_user_stack(pd: *mut u32, initial_size: u32) -> u32 {
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
pub unsafe extern "C" fn pf_get_stats() -> PfStats {
    G_STATS
}

//public api
#[unsafe(no_mangle)]
pub unsafe extern "C" fn pf_print_stats() {
    let mut buf = [0u8; 16];
    kprint(b"[PF] === Page Fault Statistics ===\n\0".as_ptr());

    macro_rules! pf_stat {
        ($label:expr, $field:expr) => {
            kprint($label.as_ptr());
            itoa($field as i32, buf.as_mut_ptr());
            kprint(buf.as_ptr());
            kprint(b"\n\0".as_ptr());
        };
    }

    pf_stat!(b"  total_faults: \0", G_STATS.total_faults);
    pf_stat!(b"  demand_allocs: \0", G_STATS.demand_allocs);
    pf_stat!(b"  cow_copies: \0", G_STATS.cow_copies);
    pf_stat!(b"  stack_grows: \0", G_STATS.stack_grows);
    pf_stat!(b"  zero_pages: \0", G_STATS.zero_pages);
    pf_stat!(b"  swap_ins: \0", G_STATS.swap_ins);
    pf_stat!(b"  prot_faults: \0", G_STATS.protection_faults);
    pf_stat!(b"  invalid_access: \0", G_STATS.invalid_access);
}