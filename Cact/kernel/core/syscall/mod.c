#include "syscalls.h"
#include "mod.h"
#include "helper.h"
#include "task.h"

// Category headers — declare the remaining core handlers
#include "process/proc.h"    // fork/exec/exit/waitpid
#include "process/signal.h"  // sigreturn
#include "io/fd.h"           // open/close/read/write/ioctl/poll
#include "mem/mm.h"          // brk/mmap/munmap/mprotect

// Exported to Rust sigreturn trampoline via FFI
const uint32_t sys_sigreturn_num = SYS_SIGRETURN;

// Syscall function pointer type — cast to/from specific signatures
typedef int (*syscall_fn)(void*, void*, void*);

// The minimal syscall table — indexed by syscall_num_t enum values.
// Everything else is served through VFS nodes (see ioctl_abi.h).
static syscall_fn syscall_table[SYSCALL_COUNT] = {
    [SYS_OPEN]      = (syscall_fn)sys_open,
    [SYS_CLOSE]     = (syscall_fn)sys_close,
    [SYS_READ]      = (syscall_fn)sys_read,
    [SYS_WRITE]     = (syscall_fn)sys_write,
    [SYS_IOCTL]     = (syscall_fn)sys_ioctl,
    [SYS_POLL]      = (syscall_fn)sys_poll,
    [SYS_FORK]      = (syscall_fn)sys_fork,
    [SYS_EXEC]      = (syscall_fn)sys_exec,
    [SYS_EXIT]      = (syscall_fn)sys_exit,
    [SYS_WAITPID]   = (syscall_fn)sys_waitpid,
    [SYS_BRK]       = (syscall_fn)sys_brk,
    [SYS_MMAP]      = (syscall_fn)sys_mmap,
    [SYS_MUNMAP]    = (syscall_fn)sys_munmap,
    [SYS_MPROTECT]  = (syscall_fn)sys_mprotect,
    [SYS_SIGRETURN] = (syscall_fn)sys_sigreturn,
};

// Syscalls that take a struct syscall_frame* instead of scalar arguments.
// All others receive (ebx, ecx, edx) directly.
static int _needs_frame(uint32_t n) {
    switch ((syscall_num_t)n) {
    case SYS_IOCTL:
    case SYS_POLL:
    case SYS_FORK:
    case SYS_EXEC:
    case SYS_EXIT:
    case SYS_WAITPID:
    case SYS_BRK:
    case SYS_MMAP:
    case SYS_MUNMAP:
    case SYS_MPROTECT:
    case SYS_SIGRETURN:
        return 1;
    default:
        return 0;
    }
}

// Entry point from the asm sysenter/syscall stubs.
void syscall_handler(struct syscall_frame* regs) {
    uint32_t num = regs->eax;
    if (num >= SYSCALL_COUNT || !syscall_table[num]) {
        regs->eax = (uint32_t)-1;
        return;
    }

    int ret;
    if (_needs_frame(num)) {
        ret = ((int(*)(struct syscall_frame*))syscall_table[num])(regs);
    } else {
        ret = syscall_table[num](
            (void*)regs->ebx,
            (void*)regs->ecx,
            (void*)regs->edx
        );
    }

    // exit() does not return a value to userspace
    if (num == SYS_EXIT) return;
    regs->eax = (uint32_t)ret;

    // Deliver any pending signal before returning to Ring 3
    if (current_task && !current_task->is_kernel)
        deliver_pending_signal(current_task, regs);
}
