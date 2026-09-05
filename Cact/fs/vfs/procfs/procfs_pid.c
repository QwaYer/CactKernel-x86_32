#include "procfs.h"
#include "procfs_internal.h"
#include "ioctl_abi.h"
#include "vfs.h"
#include "kernel.h"
#include "klib.h"
#include "memory.h"
#include "task.h"

// procfs_pid.c — dynamic /proc/<pid>/ directories.
//
// Each numeric directory exposes:
//   /proc/<pid>/info   binary cact_proc_info_t (read-only)
//   /proc/<pid>/cwd    current working directory text (read-only)
//
// Nodes are materialised on demand during path resolution and live for the
// lifetime of the kernel (small, infrequent allocations).

typedef struct pid_proc {
    uint32_t   pid;
    vfs_node_t dir;
    vfs_node_t info;
    vfs_node_t cwd;
} pid_proc_t;

static int _emit(const void *src, uint32_t len, uint32_t off, uint32_t size,
                 char *buf) {
    if (off >= len) return 0;
    uint32_t n = len - off;
    if (size < n) n = size;
    memcpy(buf, (const char *)src + off, n);
    return (int)n;
}

// Fill cact_proc_info_t for an arbitrary pid. Returns 1 if the task exists.
static int _fill_pid_info(uint32_t pid, cact_proc_info_t *out) {
    memset(out, 0, sizeof(cact_proc_info_t));
    int ok = 0;

    irq_spinlock_acquire(&scheduler_lock);
    struct task_struct *t = (struct task_struct *)task_list_head;
    while (t) {
        if (t->pid == pid) {
            out->pid  = t->pid;
            out->ppid = t->proc ? t->proc->parent_pid : 0;
            out->state = (uint32_t)t->state;
            out->flags = t->is_kernel ? 1u : 0u;
            if (t->proc) {
                out->pgid  = t->proc->pgid;
                out->sid   = t->proc->sid;
                out->uid   = t->proc->uid;
                out->gid   = t->proc->gid;
                out->euid  = t->proc->euid;
                out->egid  = t->proc->egid;
                out->umask = t->proc->umask;
            }
            ok = 1;
            break;
        }
        t = t->next;
    }
    irq_spinlock_release(&scheduler_lock);
    return ok;
}

static int _pid_info_read(vfs_node_t *node, uint32_t off, uint32_t size,
                          char *buf) {
    pid_proc_t *pc = (pid_proc_t *)node->priv;
    if (!pc) return 0;
    cact_proc_info_t info;
    if (!_fill_pid_info(pc->pid, &info)) return 0;
    return _emit(&info, sizeof(info), off, size, buf);
}

static int _pid_cwd_read(vfs_node_t *node, uint32_t off, uint32_t size,
                         char *buf) {
    pid_proc_t *pc = (pid_proc_t *)node->priv;
    if (!pc) return 0;

    char tmp[256];
    tmp[0] = '\0';
    irq_spinlock_acquire(&scheduler_lock);
    struct task_struct *t = (struct task_struct *)task_list_head;
    while (t) {
        if (t->pid == pc->pid && t->proc) {
            int i = 0;
            while (t->proc->cwd[i] && i < 255) { tmp[i] = t->proc->cwd[i]; i++; }
            tmp[i] = '\0';
            break;
        }
        t = t->next;
    }
    irq_spinlock_release(&scheduler_lock);

    uint32_t len = 0;
    while (tmp[len]) len++;
    return _emit(tmp, len, off, size, buf);
}

static vfs_ops_t pid_info_ops = { .read = _pid_info_read };
static vfs_ops_t pid_cwd_ops  = { .read = _pid_cwd_read };

static vfs_dirent_t _pid_de;

static vfs_node_t *_pid_walk(vfs_node_t *dir, const char *name) {
    (void)dir;
    if (streq(name, "info")) return &((pid_proc_t *)dir->priv)->info;
    if (streq(name, "cwd"))  return &((pid_proc_t *)dir->priv)->cwd;
    return 0;
}

static vfs_dirent_t *_pid_readdir(vfs_node_t *dir, uint32_t index) {
    (void)dir;
    const char *name = 0;
    switch (index) {
    case 0: name = "info"; break;
    case 1: name = "cwd";  break;
    default: return 0;
    }
    strlcpy(_pid_de.name, name, 128);
    _pid_de.inode = index + 1;
    return &_pid_de;
}

static void _pid_listdir(vfs_node_t *dir) {
    (void)dir;
    printk("  info\n  cwd\n");
}

static vfs_ops_t pid_dir_ops = {
    .walk    = _pid_walk,
    .readdir = _pid_readdir,
    .listdir = _pid_listdir,
};

// Create (or fail) the node tree for a live pid.
vfs_node_t *_pid_dir_get(uint32_t pid) {
    cact_proc_info_t probe;
    if (!_fill_pid_info(pid, &probe)) return 0;   // no such task

    pid_proc_t *pc = (pid_proc_t *)kmalloc(sizeof(pid_proc_t));
    if (!pc) return 0;
    memset(pc, 0, sizeof(pid_proc_t));
    pc->pid = pid;

    char nb[12];
    snprintf(nb, sizeof(nb), "%u", pid);

    memset(&pc->dir, 0, sizeof(vfs_node_t));
    strlcpy(pc->dir.name, nb, 128);
    pc->dir.type = VFS_DIRECTORY;
    pc->dir.ops  = &pid_dir_ops;
    pc->dir.priv = pc;

    memset(&pc->info, 0, sizeof(vfs_node_t));
    strlcpy(pc->info.name, "info", 128);
    pc->info.type = VFS_FILE;
    pc->info.ops  = &pid_info_ops;
    pc->info.priv = pc;

    memset(&pc->cwd, 0, sizeof(vfs_node_t));
    strlcpy(pc->cwd.name, "cwd", 128);
    pc->cwd.type = VFS_FILE;
    pc->cwd.ops  = &pid_cwd_ops;
    pc->cwd.priv = pc;

    return &pc->dir;
}

// Return the pid of the idx-th non-zero task in the list, or 0.
uint32_t _pid_dir_at(uint32_t idx) {
    uint32_t pid = 0;
    irq_spinlock_acquire(&scheduler_lock);
    struct task_struct *t = (struct task_struct *)task_list_head;
    uint32_t k = 0;
    while (t) {
        if (t->pid != 0) {
            if (k == idx) { pid = t->pid; break; }
            k++;
        }
        t = t->next;
    }
    irq_spinlock_release(&scheduler_lock);
    return pid;
}

void procfs_pid_init(void) {
    /* nothing to pre-create: dirs are materialised on demand */
}
