//! sigreturn trampoline installation: a per-process user page that issues the
//! `SYS_SIGRETURN` syscall after a signal handler returns.

use crate::ffi::{self, PAGE_PRESENT, PAGE_RW, PAGE_SIZE, PAGE_USER};
use crate::task::TaskStruct;

/// # Safety
///
/// `t` must be null or a live task whose `page_directory` is a valid page directory.
#[no_mangle]
pub unsafe extern "C" fn task_setup_sigreturn(t: *mut TaskStruct) {
    if t.is_null() {
        return;
    }
    // SAFETY: `t` is a live task (non-null checked above; see # Safety), so reading its
    // `page_directory` field is in bounds.
    let pd = unsafe { (*t).page_directory };
    // SAFETY: as above, for the `proc` field.
    let proc = unsafe { (*t).proc };
    if pd.is_null() || proc.is_null() {
        return;
    }
    map_sigreturn_trampoline_on_pd(t, pd);
}

pub(crate) fn map_sigreturn_trampoline_on_pd(t: *mut TaskStruct, pd: *mut u32) {
    if t.is_null() || pd.is_null() {
        return;
    }

    let phys: *mut u8 = cact_mm::kalloc();
    if phys.is_null() {
        return;
    }
    // SAFETY: `phys` points at a freshly allocated page of exactly `PAGE_SIZE` bytes with no
    // other live reference, so a mutable slice over it is valid and exclusive.
    let page = unsafe { core::slice::from_raw_parts_mut(phys, PAGE_SIZE as usize) };

    page.fill(0);
    // SAFETY: `sys_sigreturn_num` is a kernel global holding the statically assigned syscall
    // number; reading it has no preconditions.
    let sigret_num: u32 = unsafe { ffi::sys_sigreturn_num };

    let tramp_vaddr: u32 = 0xBEFFF000;
    // sub esp, 4          — undo the `ret` that popped ret_addr
    page[0] = 0x83;
    page[1] = 0xEC;
    page[2] = 0x04;
    // mov eax, SYS_SIGRETURN
    page[3] = 0xB8;
    page[4] = (sigret_num & 0xFF) as u8;
    page[5] = ((sigret_num >> 8) & 0xFF) as u8;
    page[6] = ((sigret_num >> 16) & 0xFF) as u8;
    page[7] = ((sigret_num >> 24) & 0xFF) as u8;

    let mech = ffi::cpu_syscall_mech();
    if mech == ffi::SYSCALL_MECH_SYSCALL {
        // SYSCALL saves the return EIP into ECX itself and leaves ESP as
        // the live user stack — the kernel entry grabs both.  No register
        // setup is needed; a bare `syscall` is the whole stub.
        // Layout: sub(3)+mov eax(5)+syscall(2)+hlt(1) = 11
        page[8] = 0x0F;   // syscall
        page[9] = 0x05;
        page[10] = 0xF4;  // hlt (return label — never reached)
    } else {
        // mov ecx, esp        — ECX = return ESP (CPU steals ECX on sysenter)
        page[8] = 0x89;
        page[9] = 0xE1;
        // mov edx, imm32      — EDX = absolute address of hlt (return EIP)
        // 32-bit has no RIP-relative; must use absolute address.
        // Layout: sub(3)+mov eax(5)+mov ecx(2)+mov edx(5)+sysenter(2)+hlt(1) = 18
        // hlt is at offset 17 → abs addr = tramp_vaddr + 17
        page[10] = 0xBA;
        let hlt_addr = tramp_vaddr + 17;
        page[11] = (hlt_addr & 0xFF) as u8;
        page[12] = ((hlt_addr >> 8) & 0xFF) as u8;
        page[13] = ((hlt_addr >> 16) & 0xFF) as u8;
        page[14] = ((hlt_addr >> 24) & 0xFF) as u8;
        // sysenter
        page[15] = 0x0F;
        page[16] = 0x34;
        // hlt (return label — should never be reached)
        page[17] = 0xF4;
    }

    // SAFETY: `t` is a live task (non-null checked at entry), so reading `is_kernel` is in bounds.
    let is_kernel = unsafe { (*t).is_kernel };
    // SAFETY: as above, reading the `proc` field.
    let t_proc = unsafe { (*t).proc };

    let vmm_flags = if is_kernel != 0 {
        PAGE_PRESENT | PAGE_RW
    } else {
        PAGE_PRESENT | PAGE_RW | PAGE_USER
    };
    // SAFETY: `pd` is a valid page directory (checked at entry) and `phys` is a freshly
    // allocated page, so mapping it at `tramp_vaddr` is the documented operation.
    unsafe { cact_mm::vmm_map(pd, tramp_vaddr, phys as u32, vmm_flags as i32) };

    if !t_proc.is_null() {
        // SAFETY: `t_proc` is the task's live `ProcMeta` (non-null checked here), so storing the
        // trampoline address into it is in bounds.
        unsafe { (*t_proc).sigreturn_trampoline = tramp_vaddr };
    }
}
