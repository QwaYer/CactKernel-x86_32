//! sigreturn trampoline installation: a per-process user page that issues the
//! `SYS_SIGRETURN` syscall after a signal handler returns.

use crate::ffi::{self, PAGE_PRESENT, PAGE_RW, PAGE_SIZE, PAGE_USER};
use crate::task::TaskStruct;

#[no_mangle]
pub unsafe extern "C" fn task_setup_sigreturn(t: *mut TaskStruct) {
    if t.is_null() || (*t).page_directory.is_null() || (*t).proc.is_null() {
        return;
    }
    map_sigreturn_trampoline_on_pd(t, (*t).page_directory);
}

pub(crate) fn map_sigreturn_trampoline_on_pd(t: *mut TaskStruct, pd: *mut u32) {
    if t.is_null() || pd.is_null() {
        return;
    }

    unsafe {
        let tramp_vaddr: u32 = 0xBEFFF000;
        let phys = ffi::kalloc() as *mut u8;
        if phys.is_null() {
            return;
        }

        for i in 0..(PAGE_SIZE as usize) {
            *phys.add(i) = 0;
        }
        let sigret_num: u32 = ffi::sys_sigreturn_num;
        // sub esp, 4          — undo the `ret` that popped ret_addr
        *phys.add(0) = 0x83;
        *phys.add(1) = 0xEC;
        *phys.add(2) = 0x04;
        // mov eax, SYS_SIGRETURN
        *phys.add(3) = 0xB8;
        *phys.add(4) = (sigret_num & 0xFF) as u8;
        *phys.add(5) = ((sigret_num >> 8) & 0xFF) as u8;
        *phys.add(6) = ((sigret_num >> 16) & 0xFF) as u8;
        *phys.add(7) = ((sigret_num >> 24) & 0xFF) as u8;

        if ffi::cpu_syscall_mech() == ffi::SYSCALL_MECH_SYSCALL {
            // SYSCALL saves the return EIP into ECX itself and leaves ESP as
            // the live user stack — the kernel entry grabs both.  No register
            // setup is needed; a bare `syscall` is the whole stub.
            // Layout: sub(3)+mov eax(5)+syscall(2)+hlt(1) = 11
            *phys.add(8) = 0x0F;   // syscall
            *phys.add(9) = 0x05;
            *phys.add(10) = 0xF4;  // hlt (return label — never reached)
        } else {
            // mov ecx, esp        — ECX = return ESP (CPU steals ECX on sysenter)
            *phys.add(8) = 0x89;
            *phys.add(9) = 0xE1;
            // mov edx, imm32      — EDX = absolute address of hlt (return EIP)
            // 32-bit has no RIP-relative; must use absolute address.
            // Layout: sub(3)+mov eax(5)+mov ecx(2)+mov edx(5)+sysenter(2)+hlt(1) = 18
            // hlt is at offset 17 → abs addr = tramp_vaddr + 17
            *phys.add(10) = 0xBA;
            let hlt_addr = tramp_vaddr + 17;
            *phys.add(11) = (hlt_addr & 0xFF) as u8;
            *phys.add(12) = ((hlt_addr >> 8) & 0xFF) as u8;
            *phys.add(13) = ((hlt_addr >> 16) & 0xFF) as u8;
            *phys.add(14) = ((hlt_addr >> 24) & 0xFF) as u8;
            // sysenter
            *phys.add(15) = 0x0F;
            *phys.add(16) = 0x34;
            // hlt (return label — should never be reached)
            *phys.add(17) = 0xF4;
        }

        let vmm_flags = if (*t).is_kernel != 0 {
            PAGE_PRESENT | PAGE_RW
        } else {
            PAGE_PRESENT | PAGE_RW | PAGE_USER
        };
        ffi::vmm_map(pd, tramp_vaddr, phys as u32, vmm_flags);

        if !(*t).proc.is_null() {
            (*(*t).proc).sigreturn_trampoline = tramp_vaddr;
        }
    }
}
