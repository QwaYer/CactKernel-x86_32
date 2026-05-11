#include "syscalls.h"
#include "mod.h"
#include "helper.h"
#include "task.h"

// Category headers — each defines its own sys_* functions
#include "proc/proc.h"
#include "proc/signal.h"
#include "proc/session.h"
#include "fd/fd.h"
#include "file/file.h"
#include "path/path.h"
#include "sys/sys.h"
#include "mm/mm.h"
#include "ipc/ipc.h"
#include "time/time.h"
#include "user/user.h"
#include "net/net.h"
#include "kmod/kmod.h"

// task.h/mm.h may define legacy SYS_MMAP/etc. numbers — override with enum values
#ifdef SYS_MMAP
#undef SYS_MMAP
#endif
#ifdef SYS_MUNMAP
#undef SYS_MUNMAP
#endif
#ifdef SYS_MPROTECT
#undef SYS_MPROTECT
#endif

// Exported to Rust sigreturn trampoline via FFI
const uint32_t sys_sigreturn_num = SYS_SIGRETURN;

// Syscall function pointer type — cast to/from specific signatures
typedef int (*syscall_fn)(void*, void*, void*);

// The syscall table — indexed by syscall_num_t enum values
static syscall_fn syscall_table[SYSCALL_COUNT] = {
    // 0 — debug
    [SYS_PRINT]         = (syscall_fn)sys_print,

    // 1–7 — process
    [SYS_GETPID]        = (syscall_fn)sys_get_pid,
    [SYS_GETPPID]       = (syscall_fn)sys_getppid,
    [SYS_FORK]          = (syscall_fn)sys_fork,
    [SYS_EXEC]          = (syscall_fn)sys_exec,
    [SYS_EXIT]          = (syscall_fn)sys_exit,
    [SYS_WAITPID]       = (syscall_fn)sys_waitpid,
    [SYS_SLEEP]         = (syscall_fn)sys_sleep,

    // 8–11 — sessions / process groups
    [SYS_SETSID]        = (syscall_fn)sys_setsid,
    [SYS_SETPGID]       = (syscall_fn)sys_setpgid,
    [SYS_GETPGID]       = (syscall_fn)sys_getpgid,
    [SYS_GETPGRP]       = (syscall_fn)sys_getpgrp,

    // 12–20 — signals
    [SYS_KILL]          = (syscall_fn)sys_kill,
    [SYS_SIGNAL]        = (syscall_fn)sys_signal,
    [SYS_SIGACTION]     = (syscall_fn)sys_sigaction,
    [SYS_SIGPROCMASK]   = (syscall_fn)sys_sigprocmask,
    [SYS_SIGRETURN]     = (syscall_fn)sys_sigreturn,   // 16 — hardcoded in trampoline
    [SYS_SIGPENDING]    = (syscall_fn)sys_sigpending,
    [SYS_SIGSUSPEND]    = (syscall_fn)sys_sigsuspend,
    [SYS_ALARM]         = (syscall_fn)sys_alarm,
    [SYS_SETITIMER]     = (syscall_fn)sys_setitimer,

    // 21–32 — file descriptors
    [SYS_OPEN]          = (syscall_fn)sys_open,
    [SYS_READ]          = (syscall_fn)sys_read,
    [SYS_WRITE]         = (syscall_fn)sys_write,
    [SYS_CLOSE]         = (syscall_fn)sys_close,
    [SYS_LSEEK]         = (syscall_fn)sys_lseek,
    [SYS_IOCTL]         = (syscall_fn)sys_ioctl,
    [SYS_FCNTL]         = (syscall_fn)sys_fcntl,
    [SYS_DUP]           = (syscall_fn)sys_dup,
    [SYS_DUP2]          = (syscall_fn)sys_dup2,
    [SYS_PIPE]          = (syscall_fn)sys_pipe,
    [SYS_SELECT]        = (syscall_fn)sys_select,
    [SYS_POLL]          = (syscall_fn)sys_poll,

    // 33–43 — file metadata
    [SYS_STAT]          = (syscall_fn)sys_stat,
    [SYS_FSTAT]         = (syscall_fn)sys_fstat,
    [SYS_ACCESS]        = (syscall_fn)sys_access,
    [SYS_CHMOD]         = (syscall_fn)sys_chmod,
    [SYS_CHOWN]         = (syscall_fn)sys_chown,
    [SYS_UMASK]         = (syscall_fn)sys_umask,
    [SYS_TRUNCATE]      = (syscall_fn)sys_truncate,
    [SYS_FTRUNCATE]     = (syscall_fn)sys_ftruncate,
    [SYS_SYNC]          = (syscall_fn)sys_sync,
    [SYS_FSYNC]         = (syscall_fn)sys_fsync,
    [SYS_MKNOD]         = (syscall_fn)sys_mknod,

    // 44–56 — directories and paths
    [SYS_CREATE]        = (syscall_fn)sys_create,
    [SYS_MKDIR]         = (syscall_fn)sys_mkdir,
    [SYS_RMDIR]         = (syscall_fn)sys_rmdir,
    [SYS_DELETE]        = (syscall_fn)sys_delete,
    [SYS_UNLINK]        = (syscall_fn)sys_unlink,
    [SYS_RENAME]        = (syscall_fn)sys_rename,
    [SYS_LINK]          = (syscall_fn)sys_link,
    [SYS_SYMLINK]       = (syscall_fn)sys_symlink,
    [SYS_READLINK]      = (syscall_fn)sys_readlink,
    [SYS_GETDENTS]      = (syscall_fn)sys_getdents,
    [SYS_CHDIR]         = (syscall_fn)sys_chdir,
    [SYS_GETCWD]        = (syscall_fn)sys_getcwd,
    [SYS_CHROOT]        = (syscall_fn)sys_chroot,

    // 57–60 — system operations
    [SYS_MOUNT]         = (syscall_fn)sys_mount,
    [SYS_UMOUNT]        = (syscall_fn)sys_umount,
    [SYS_REBOOT]        = (syscall_fn)sys_reboot,
    [SYS_UNAME]         = (syscall_fn)sys_uname,

    // 61–64 — memory
    [SYS_BRK]           = (syscall_fn)sys_brk,
    [SYS_MMAP]          = (syscall_fn)sys_mmap,
    [SYS_MUNMAP]        = (syscall_fn)sys_munmap,
    [SYS_MPROTECT]      = (syscall_fn)sys_mprotect,

    // 65–68 — IPC / SHM
    [SYS_SHMGET]        = (syscall_fn)sys_shmget,
    [SYS_SHMAT]         = (syscall_fn)sys_shmat,
    [SYS_SHMDT]         = (syscall_fn)sys_shmdt,
    [SYS_SHMCTL]        = (syscall_fn)sys_shmctl,

    // 69–71 — time
    [SYS_GETTIMEOFDAY]  = (syscall_fn)sys_gettimeofday,
    [SYS_CLOCK_GETTIME] = (syscall_fn)sys_clock_gettime,
    [SYS_NANOSLEEP]     = (syscall_fn)sys_nanosleep,

    // 72–77 — identification
    [SYS_GETUID]        = (syscall_fn)sys_getuid,
    [SYS_GETGID]        = (syscall_fn)sys_getgid,
    [SYS_GETEUID]       = (syscall_fn)sys_geteuid,
    [SYS_GETEGID]       = (syscall_fn)sys_getegid,
    [SYS_SETUID]        = (syscall_fn)sys_setuid,
    [SYS_SETGID]        = (syscall_fn)sys_setgid,

    // 78–89 — network
    [SYS_SOCKET]        = (syscall_fn)sys_socket,
    [SYS_BIND]          = (syscall_fn)sys_bind,
    [SYS_CONNECT]       = (syscall_fn)sys_connect,
    [SYS_LISTEN]        = (syscall_fn)sys_listen,
    [SYS_ACCEPT]        = (syscall_fn)sys_accept,
    [SYS_SEND]          = (syscall_fn)sys_send,
    [SYS_RECV]          = (syscall_fn)sys_recv,
    [SYS_SENDTO]        = (syscall_fn)sys_sendto,
    [SYS_RECVFROM]      = (syscall_fn)sys_recvfrom,
    [SYS_SHUTDOWN]      = (syscall_fn)sys_shutdown,
    [SYS_SETSOCKOPT]    = (syscall_fn)sys_setsockopt,
    [SYS_GETSOCKOPT]    = (syscall_fn)sys_getsockopt,
    [SYS_PING_ECHO]     = (syscall_fn)sys_ping_echo,
    [SYS_NETCFG_SET]    = (syscall_fn)sys_netcfg_set,

    [SYS_MODULE_LOAD]   = (syscall_fn)sys_module_load,
    [SYS_MODULE_UNLOAD] = (syscall_fn)sys_module_unload,
};

// Syscalls that take a struct syscall_frame* instead of three scalar arguments.
// All others receive (ebx, ecx, edx) directly.
static int _needs_frame(uint32_t n) {
    switch ((syscall_num_t)n) {
    // process
    case SYS_FORK:
    case SYS_EXEC:
    case SYS_EXIT:
    case SYS_WAITPID:
    case SYS_SLEEP:
    // signals
    case SYS_SIGACTION:
    case SYS_SIGPROCMASK:
    case SYS_SIGRETURN:
    case SYS_SIGPENDING:
    case SYS_SIGSUSPEND:
    case SYS_ALARM:
    case SYS_SETITIMER:
    // fd
    case SYS_LSEEK:
    case SYS_IOCTL:
    case SYS_DUP2:
    case SYS_PIPE:
    case SYS_SELECT:
    case SYS_POLL:
    // metadata
    case SYS_STAT:
    case SYS_FSTAT:
    case SYS_GETDENTS:
    // paths
    case SYS_SYMLINK:
    case SYS_READLINK:
    case SYS_LINK:
    case SYS_CHDIR:
    case SYS_GETCWD:
    // memory
    case SYS_BRK:
    case SYS_MMAP:
    case SYS_MUNMAP:
    case SYS_MPROTECT:
    // SHM
    case SYS_SHMGET:
    case SYS_SHMAT:
    case SYS_SHMDT:
    case SYS_SHMCTL:
    // time
    case SYS_GETTIMEOFDAY:
    case SYS_CLOCK_GETTIME:
    case SYS_NANOSLEEP:
    // network
    case SYS_SOCKET:
    case SYS_BIND:
    case SYS_CONNECT:
    case SYS_LISTEN:
    case SYS_ACCEPT:
    case SYS_SEND:
    case SYS_RECV:
    case SYS_SENDTO:
    case SYS_RECVFROM:
    case SYS_SHUTDOWN:
    case SYS_SETSOCKOPT:
    case SYS_GETSOCKOPT:
    case SYS_PING_ECHO:
    case SYS_NETCFG_SET:
        return 1;
    default:
        return 0;
    }
}

// Entry point from interrupt.asm (int 0x80)
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