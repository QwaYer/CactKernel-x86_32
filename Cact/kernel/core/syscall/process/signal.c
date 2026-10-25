#include "signal.h"
#include "validate.h"
#include "helper.h"
#include "klib.h"
#include "ioctl_abi.h"   // CACT_SS_ONSTACK

// The kernel keeps exactly one signal-related trap: sigreturn, which restores
// the user context saved on the signal frame stack.  All signal *control*
// (sigaction/procmask/kill/alarm/setitimer/sigsuspend/...) is served by the
// /proc/self/ctl node ioctls (CACT_PROCCTL_*).

// sigreturn() — restore user context from the signal frame on the stack
/* Queue `signal` for `pid` and remember what it came from, so an SA_SIGINFO
 * handler gets a truthful si_code (and, for a hardware fault, the address that
 * faulted).  A plain task_signal() leaves the default, SI_USER.
 *
 * `signal` is the bit (SIGSEGV = 1<<8, as the kernel's own headers define it);
 * the per-signal arrays are indexed by the bit *number*. */
void task_signal_code(uint32_t pid, uint32_t signal, uint32_t code, uint32_t addr) {
    if (current_task && current_task->proc && current_task->pid == pid) {
        int bit = 0;
        uint32_t m = signal;
        while (bit < NSIG && !(m & 1u)) { m >>= 1; bit++; }
        if (bit < NSIG) {
            current_task->proc->signal_codes[bit] = code;
            current_task->proc->signal_addr      = addr;
        }
    }
    task_signal(pid, signal);
}

static int sys_sigreturn_impl(struct syscall_frame* regs) {
    if (!current_task || current_task->is_kernel) return -1;

    uint32_t user_esp = regs->useresp;
    if (user_esp < USER_SPACE_START || user_esp >= KERNEL_BASE) return -1;

    uint32_t page_off = user_esp & 0xFFFu;
    if (page_off + sizeof(signal_frame_t) > PAGE_SIZE) return -1;

    signal_frame_t frame_buf;

    // Read frame with double-check of page tables to prevent TOCTOU
    for (int attempt = 0; attempt < 2; attempt++) {
        uint32_t* pd = current_task->page_directory;
        if (!pd) return -1;
        uint32_t pdi = PD_INDEX(user_esp);
        uint32_t pti = PT_INDEX(user_esp);
        if (!(pd[pdi] & PAGE_PRESENT)) return -1;

        uint32_t* pt = (uint32_t*)(pd[pdi] & ~0xFFFu);
        if (!(pt[pti] & PAGE_PRESENT)) return -1;

        uint32_t phys = pt[pti] & ~0xFFFu;

        memory_copy(&frame_buf, (signal_frame_t*)(phys + page_off), sizeof(frame_buf));

        // Re-validate: PTE must still point to the same physical page
        if ((pt[pti] & ~0xFFFu) == phys && (pt[pti] & PAGE_PRESENT))
            break;

        if (attempt == 1) return -1;
    }

    regs->eax     = frame_buf.eax;
    regs->ecx     = frame_buf.ecx;
    regs->edx     = frame_buf.edx;
    regs->ebx     = frame_buf.ebx;
    regs->ebp     = frame_buf.ebp;
    regs->esi     = frame_buf.esi;
    regs->edi     = frame_buf.edi;
    regs->eip     = frame_buf.eip;
    regs->eflags  = frame_buf.eflags;
    regs->useresp = frame_buf.esp;

    // Put back the mask the handler interrupted — delivery blocked the signal
    // itself (unless SA_NODEFER), and any signal that arrived meanwhile must
    // become deliverable again.
    current_task->proc->signal_mask = frame_buf.saved_mask;

    // The handler is leaving, so the alternate signal stack is free again: a
    // later SA_ONSTACK delivery may use it (and sigaltstack() should stop
    // reporting SS_ONSTACK).
    current_task->proc->altstack_flags &= ~(uint32_t)CACT_SS_ONSTACK;

    return 0;
}

int sys_sigreturn(struct syscall_frame* regs) { return sys_sigreturn_impl(regs); }
