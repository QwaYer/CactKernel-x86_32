#include "helper.h"
#include "validate.h"
#include "memory.h"

// Find a free file descriptor slot (starting from 3) and install the file.
int alloc_fd(vfs_node_t *node) {
    if (!node || !current_task) return -1;
    file_t *f = file_alloc(node);
    if (!f) return -1;
    for (int i = 3; i < MAX_FD; i++) {
        if (!current_task->proc->fds->files[i]) {
            current_task->proc->fds->files[i] = f;
            return i;
        }
    }
    // No free slot — undo the allocation
    file_unref(f);
    return -1;
}

// Deliver a pending signal to a task by constructing a signal frame on its
// user stack and redirecting EIP to the signal handler.
// Called from syscall_handler before returning to userspace.
void deliver_pending_signal(struct task_struct *t, struct syscall_frame *regs) {
    uint32_t deliverable = t->proc->pending_signals & ~t->proc->signal_mask;
    if (!deliverable) return;

    for (int bit = 0; bit < NSIG; bit++) {
        uint32_t mask = (1u << bit);
        if (!(deliverable & mask)) continue;
        uint32_t handler = t->proc->signal_handlers[bit];
        if (handler <= SIG_IGN) continue;
        if (handler < USER_SPACE_START || handler >= KERNEL_BASE) continue;

        uint32_t new_esp = regs->useresp - sizeof(signal_frame_t);

        if (new_esp < USER_SPACE_START || new_esp >= KERNEL_BASE) continue;
        uint32_t page_off = new_esp & 0xFFFu;
        if (page_off + sizeof(signal_frame_t) > PAGE_SIZE) continue;

        uint32_t *pd  = t->page_directory;
        if (!pd) continue;
        uint32_t  pdi = PD_INDEX(new_esp);
        uint32_t  pti = PT_INDEX(new_esp);
        if (!(pd[pdi] & PAGE_PRESENT)) continue;
        uint32_t *pt = (uint32_t *)(pd[pdi] & ~0xFFFu);
        uint32_t pte = pt[pti];
        if (!(pte & PAGE_PRESENT)) continue;
        if (!(pte & PAGE_USER))    continue;
        if (!(pte & PAGE_RW))      continue;

        signal_frame_t *frame = (signal_frame_t *)new_esp;

        frame->ret_addr = t->proc->sigreturn_trampoline;
        frame->signum   = (uint32_t)bit;
        frame->eax      = regs->eax;
        frame->ecx      = regs->ecx;
        frame->edx      = regs->edx;
        frame->ebx      = regs->ebx;
        frame->esp      = regs->useresp;
        frame->ebp      = regs->ebp;
        frame->esi      = regs->esi;
        frame->edi      = regs->edi;
        frame->eip      = regs->eip;
        frame->eflags   = regs->eflags;

        regs->useresp = new_esp;
        regs->eip     = handler;

        t->proc->pending_signals &= ~mask;
        return;
    }
}
