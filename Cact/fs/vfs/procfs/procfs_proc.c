#include "procfs.h"
#include "procfs_internal.h"
#include "ioctl_abi.h"
#include "vfs.h"
#include "kernel.h"
#include "klib.h"
#include "memory.h"
#include "task.h"
#include "shm.h"
#include "validate.h"

// procfs_proc.c — per-process and system service nodes under /proc.
//
// These nodes exist so userspace (CactLib) can obtain and mutate per-process
// state without a dedicated syscall: reading a node or running an ioctl on it
// goes through the ordinary open/read/ioctl syscall path.  The node content is
// resolved dynamically against current_task at access time.
//
//   /proc/self       directory for the calling process
//   /proc/self/info  binary cact_proc_info_t (read-only)
//   /proc/self/cwd   current working directory text (read-only)
//   /proc/self/ctl   process control ioctls (setsid/setpgid/setuid/...)

static vfs_node_t self_info_node;
static vfs_node_t self_cwd_node;
static vfs_node_t self_ctl_node;

vfs_node_t proc_self_dir;   // exported for the procfs root walk

// Copy a fixed-size kernel buffer with file offset semantics into the
// (pre-validated) user buffer supplied by read_vfs.
static int _emit(const void *src, uint32_t len, uint32_t off, uint32_t size,
                 char *buf) {
    if (off >= len) return 0;
    uint32_t n = len - off;
    if (size < n) n = size;
    memcpy(buf, (const char *)src + off, n);
    return (int)n;
}

// Fill the info struct from the calling process.
static void _fill_self_info(cact_proc_info_t *info) {
    memset(info, 0, sizeof(cact_proc_info_t));
    if (!current_task) return;

    info->pid  = current_task->pid;
    info->ppid = current_task->proc ? current_task->proc->parent_pid : 0;
    info->state = (uint32_t)current_task->state;
    info->flags = current_task->is_kernel ? 1u : 0u;

    if (!current_task->proc) return;
    info->pgid  = current_task->proc->pgid;
    info->sid   = current_task->proc->sid;
    info->uid   = current_task->proc->uid;
    info->gid   = current_task->proc->gid;
    info->euid  = current_task->proc->euid;
    info->egid  = current_task->proc->egid;
    info->umask = current_task->proc->umask;
}

static int _info_read(vfs_node_t *node, uint32_t off, uint32_t size,
                      char *buf) {
    (void)node;
    cact_proc_info_t info;
    _fill_self_info(&info);
    return _emit(&info, sizeof(info), off, size, buf);
}

static int _cwd_read(vfs_node_t *node, uint32_t off, uint32_t size,
                     char *buf) {
    (void)node;
    if (!current_task || !current_task->proc) return 0;
    uint32_t len = 0;
    while (current_task->proc->cwd[len] && len < sizeof(current_task->proc->cwd))
        len++;
    return _emit(current_task->proc->cwd, len, off, size, buf);
}

// ---- /proc/self/ctl ioctl handler --------------------------------------

#ifndef EPERM
#define EPERM 1
#endif
#ifndef ESRCH
#define ESRCH 3
#endif
#ifndef EINVAL
#define EINVAL 22
#endif
#ifndef EFAULT
#define EFAULT 14
#endif
#ifndef ENOTDIR
#define ENOTDIR 20
#endif

extern uint32_t timer_ticks_get(void);
extern void schedule(void);

// Signal masking bits (same scheme as proc->pending_signals/signal_mask).
static const uint32_t _SIG_UNCATCHABLE = 0x00000003u;   // SIGKILL | SIGSTOP

// May the calling task signal |pid|?  0 = yes, negative = -errno.
static int _sig_permitted(uint32_t pid) {
    if (current_task->proc->euid == 0) {
        if (pid == 0 || pid == current_task->pid) return 0;
        struct task_struct *t = task_list_head;
        while (t) {
            if (t->pid == pid) return 0;
            t = t->next;
        }
        return -ESRCH;
    }
    if (pid != current_task->pid) {
        struct task_struct *t = task_list_head;
        while (t) {
            if (t->pid == pid)
                return (t->proc && t->proc->uid == current_task->proc->uid)
                       ? 0 : -EPERM;
            t = t->next;
        }
        return -ESRCH;
    }
    return 0;
}

// chdir() with a kernel-side path string.
static int _self_chdir(const char *path) {
    if (!path || !path[0]) return -EINVAL;

    char abs[256];
    if (path[0] == '/') {
        int i = 0;
        while (path[i] && i < 255) { abs[i] = path[i]; i++; }
        abs[i] = '\0';
    } else {
        int p = 0;
        for (int i = 0; current_task->proc->cwd[i] && p < 254; i++)
            abs[p++] = current_task->proc->cwd[i];
        if (p > 0 && abs[p-1] != '/') abs[p++] = '/';
        for (int i = 0; path[i] && p < 255; i++)
            abs[p++] = path[i];
        abs[p] = '\0';
    }

    int segs_start[64], segs_len[64];
    int nseg = 0;
    const char *s = abs;
    while (*s) {
        while (*s == '/') s++;
        if (!*s) break;
        const char *seg = s;
        int slen = 0;
        while (*s && *s != '/') { s++; slen++; }
        if (slen == 1 && seg[0] == '.') continue;
        if (slen == 2 && seg[0] == '.' && seg[1] == '.') {
            if (nseg > 0) nseg--;
            continue;
        }
        if (nseg >= 64) break;
        segs_start[nseg] = (int)(seg - abs);
        segs_len[nseg]   = slen;
        nseg++;
    }

    char norm[256];
    if (nseg == 0) {
        norm[0] = '/'; norm[1] = '\0';
    } else {
        int p = 0;
        for (int i = 0; i < nseg && p < 254; i++) {
            norm[p++] = '/';
            for (int j = 0; j < segs_len[i] && p < 255; j++)
                norm[p++] = abs[segs_start[i] + j];
        }
        norm[p] = '\0';
    }

    vfs_node_t *node = vfs_walk_path(vfs_root, norm);
    if (!node || node->type != VFS_DIRECTORY) return -ENOTDIR;

    int i = 0;
    while (norm[i] && i < 255) { current_task->proc->cwd[i] = norm[i]; i++; }
    current_task->proc->cwd[i] = '\0';
    return 0;
}

static int _self_ctl_ioctl(vfs_node_t *node, uint32_t cmd, void *arg) {
    (void)node;
    if (!current_task || !current_task->proc) return -1;

    switch (cmd) {
    case CACT_PROCCTL_SETSID: {
        current_task->proc->sid  = current_task->pid;
        current_task->proc->pgid = current_task->pid;
        return (int)current_task->proc->sid;
    }

    case CACT_PROCCTL_SETPGID: {
        cact_pgid_arg_t a;
        if (!arg) return -1;
        if (copy_from_user(&a, arg, sizeof(a)) != 0) return -1;

        struct task_struct *t;
        if (a.pid == 0 || a.pid == current_task->pid) {
            t = current_task;
        } else {
            if (current_task->proc->euid != 0) return -1;   // EPERM
            t = 0;
            struct task_struct *cur = task_list_head;
            while (cur) {
                if (cur->pid == a.pid) { t = cur; break; }
                cur = cur->next;
            }
            if (!t || !t->proc) return -3;                  // ESRCH
        }
        uint32_t pgid = (a.pgid == 0) ? t->pid : a.pgid;
        t->proc->pgid = pgid;
        return 0;
    }

    case CACT_PROCCTL_SETUID: {
        uint32_t uid;
        if (!arg) return -1;
        if (copy_from_user(&uid, arg, sizeof(uid)) != 0) return -1;
        if (current_task->proc->euid != 0 && uid != current_task->proc->uid)
            return -1;
        current_task->proc->uid  = uid;
        current_task->proc->euid = uid;
        return 0;
    }

    case CACT_PROCCTL_SETGID: {
        uint32_t gid;
        if (!arg) return -1;
        if (copy_from_user(&gid, arg, sizeof(gid)) != 0) return -1;
        if (current_task->proc->euid != 0 && gid != current_task->proc->gid)
            return -1;
        current_task->proc->gid  = gid;
        current_task->proc->egid = gid;
        return 0;
    }

    case CACT_PROCCTL_UMASK: {
        uint32_t mask;
        if (!arg) return -1;
        if (copy_from_user(&mask, arg, sizeof(mask)) != 0) return -1;
        uint32_t old = current_task->proc->umask;
        current_task->proc->umask = mask & 0777;
        return (int)old;
    }

    case CACT_PROCCTL_CHDIR: {
        if (!validate_user_str((const char *)arg)) return -EFAULT;
        char *k = copy_path_from_user((const char *)arg);
        if (!k) return -EFAULT;
        int r = _self_chdir(k);
        kfree(k);
        return r;
    }

    case CACT_PROCCTL_CHROOT: {
        if (current_task->proc->euid != 0) return -EPERM;
        if (!validate_user_str((const char *)arg)) return -EFAULT;
        char *k = copy_path_from_user((const char *)arg);
        if (!k) return -EFAULT;
        vfs_node_t *n = vfs_resolve_path(k);
        kfree(k);
        if (!n || n->type != VFS_DIRECTORY) return -ENOTDIR;
        current_task->proc->root = n;
        return 0;
    }

    case CACT_PROCCTL_SIGNAL: {
        cact_signal_arg_t a;
        if (!arg) return -EINVAL;
        if (copy_from_user(&a, arg, sizeof(a)) != 0) return -EFAULT;
        int p = _sig_permitted(a.pid);
        if (p) return p;
        /* signum — индекс сигнала ядра (бит маски). 0 = SIGKILL (принудительно). */
        if (a.signum == 0) { task_kill(a.pid); return 0; }
        if (a.signum >= NSIG) return -EINVAL;
        task_signal(a.pid, a.signum);
        return 0;
    }

    case CACT_PROCCTL_SIGACTION: {
        cact_sigaction_arg_t a;
        if (!arg) return -EINVAL;
        if (copy_from_user(&a, arg, sizeof(a)) != 0) return -EFAULT;
        if (a.signum >= NSIG) return -EINVAL;
        // Reject handlers in kernel space (SIG_DFL=0 / SIG_IGN=1 allowed).
        if (a.handler > 1 &&
            (a.handler < USER_SPACE_START || a.handler >= KERNEL_BASE))
            return -EINVAL;
        return task_sigaction(current_task, a.signum, a.handler);
    }

    case CACT_PROCCTL_SIGPROCMASK: {
        cact_sigprocmask_arg_t a;
        if (!arg) return -EINVAL;
        if (copy_from_user(&a, arg, sizeof(a)) != 0) return -EFAULT;
        uint32_t old = current_task->proc->signal_mask;
        switch (a.how) {
        case 0: current_task->proc->signal_mask = old | a.set;      break;  // SIG_BLOCK
        case 1: current_task->proc->signal_mask = old & ~a.set;     break;  // SIG_UNBLOCK
        case 2: current_task->proc->signal_mask = a.set;            break;  // SIG_SETMASK
        default: return -EINVAL;
        }
        a.oldset = old;
        return copy_to_user(arg, &a, sizeof(a));
    }

    case CACT_PROCCTL_SIGPENDING: {
        if (!arg || !validate_user_ptr(arg, sizeof(uint32_t))) return -EFAULT;
        *(uint32_t *)arg = current_task->proc->pending_signals &
                           current_task->proc->signal_mask;
        return 0;
    }

    case CACT_PROCCTL_SIGSUSPEND: {
        uint32_t sigmask;
        if (!arg) return -EINVAL;
        if (copy_from_user(&sigmask, arg, sizeof(sigmask)) != 0) return -EFAULT;
        if (current_task->is_kernel) return -1;
        current_task->proc->saved_signal_mask = current_task->proc->signal_mask;
        current_task->proc->signal_mask = sigmask & ~_SIG_UNCATCHABLE;
        current_task->proc->in_sigsuspend = 1;
        current_task->state = TASK_SLEEPING;
        schedule();   // woken by a delivered signal
        return -1;
    }

    case CACT_PROCCTL_ALARM: {
        uint32_t seconds;
        if (!arg) return -EINVAL;
        if (copy_from_user(&seconds, arg, sizeof(seconds)) != 0) return -EFAULT;
        uint32_t now = timer_ticks_get();
        uint32_t remaining = 0;
        if (current_task->proc->alarm_ticks) {
            uint32_t left = (current_task->proc->alarm_ticks > now)
                            ? (current_task->proc->alarm_ticks - now) : 0;
            remaining = (left + 99) / 100;   // 100 Hz ticks -> seconds
        }
        if (seconds == 0)
            current_task->proc->alarm_ticks = 0;
        else
            current_task->proc->alarm_ticks = now + seconds * 100;
        return (int)remaining;
    }

    case CACT_PROCCTL_SETITIMER: {
        cact_itimerval_arg_t a;
        if (!arg) return -EINVAL;
        if (copy_from_user(&a, arg, sizeof(a)) != 0) return -EFAULT;
        uint32_t now = timer_ticks_get();

        if (current_task->proc->itimer_value &&
            current_task->proc->itimer_value > now) {
            uint32_t left = current_task->proc->itimer_value - now;
            a.old_value_ms = left * 10;   // 100 Hz ticks -> ms
        } else {
            a.old_value_ms = 0;
        }
        a.old_interval_ms = current_task->proc->itimer_interval * 10;

        uint32_t val_ticks = (a.it_value_ms + 9) / 10;
        uint32_t int_ticks = (a.it_interval_ms + 9) / 10;
        if (val_ticks == 0) {
            current_task->proc->itimer_value    = 0;
            current_task->proc->itimer_interval = 0;
        } else {
            current_task->proc->itimer_value    = now + val_ticks;
            current_task->proc->itimer_interval = int_ticks;
        }
        return copy_to_user(arg, &a, sizeof(a));
    }

    case CACT_PROCCTL_SHMGET: {
        cact_shmget_arg_t a;
        if (!arg) return -EINVAL;
        if (copy_from_user(&a, arg, sizeof(a)) != 0) return -EFAULT;
        return shm_get((int)a.key, a.size, (int)a.flags);
    }

    case CACT_PROCCTL_SHMAT: {
        cact_shmat_arg_t a;
        if (!arg) return -EINVAL;
        if (copy_from_user(&a, arg, sizeof(a)) != 0) return -EFAULT;
        uint32_t addr = shm_at((int)a.shmid, a.addr, (int)a.flags);
        a.addr = addr;
        return copy_to_user(arg, &a, sizeof(a));
    }

    case CACT_PROCCTL_SHMDT: {
        uint32_t addr;
        if (!arg) return -EINVAL;
        if (copy_from_user(&addr, arg, sizeof(addr)) != 0) return -EFAULT;
        return shm_dt(addr);
    }

    case CACT_PROCCTL_SHMCTL: {
        cact_shmctl_arg_t a;
        if (!arg) return -EINVAL;
        if (copy_from_user(&a, arg, sizeof(a)) != 0) return -EFAULT;
        if (a.buf && !validate_user_ptr(a.buf, 64)) return -EFAULT;
        return shm_ctl((int)a.shmid, (int)a.cmd, a.buf);
    }

    default:
        return -1;
    }
}

// ---- node ops -----------------------------------------------------------

static vfs_ops_t self_info_ops = { .read = _info_read };
static vfs_ops_t self_cwd_ops  = { .read = _cwd_read };
static vfs_ops_t self_ctl_ops  = { .ioctl = _self_ctl_ioctl };

static vfs_dirent_t _self_de;

static vfs_node_t *_self_walk(vfs_node_t *dir, const char *name) {
    (void)dir;
    if (streq(name, "info")) return &self_info_node;
    if (streq(name, "cwd"))  return &self_cwd_node;
    if (streq(name, "ctl"))  return &self_ctl_node;
    return 0;
}

static vfs_dirent_t *_self_readdir(vfs_node_t *dir, uint32_t index) {
    (void)dir;
    const char *name = 0;
    switch (index) {
    case 0:  name = "info"; break;
    case 1:  name = "cwd";  break;
    case 2:  name = "ctl";  break;
    default: return 0;
    }
    strlcpy(_self_de.name, name, 128);
    _self_de.inode = index + 1;
    return &_self_de;
}

static void _self_listdir(vfs_node_t *dir) {
    (void)dir;
    printk("  info\n  cwd\n  ctl\n");
}

static vfs_ops_t self_dir_ops = {
    .walk    = _self_walk,
    .readdir = _self_readdir,
    .listdir = _self_listdir,
};

// Register the per-process service subtree.
void procfs_proc_init(void) {
    memset(&proc_self_dir, 0, sizeof(vfs_node_t));
    strlcpy(proc_self_dir.name, "self", 128);
    proc_self_dir.type = VFS_DIRECTORY;
    proc_self_dir.ops  = &self_dir_ops;

    memset(&self_info_node, 0, sizeof(vfs_node_t));
    strlcpy(self_info_node.name, "info", 128);
    self_info_node.type = VFS_FILE;
    self_info_node.ops  = &self_info_ops;
    self_info_node.uid  = 0;

    memset(&self_cwd_node, 0, sizeof(vfs_node_t));
    strlcpy(self_cwd_node.name, "cwd", 128);
    self_cwd_node.type = VFS_FILE;
    self_cwd_node.ops  = &self_cwd_ops;

    memset(&self_ctl_node, 0, sizeof(vfs_node_t));
    strlcpy(self_ctl_node.name, "ctl", 128);
    self_ctl_node.type = VFS_FILE;
    self_ctl_node.ops  = &self_ctl_ops;
}
