#include "kernel.h"
#include "task.h"
#include "vfs.h"
#include "memory.h"
#include "mmap.h"
#include "pipe.h"
#include "socket.h"
#include "tcp.h"
#include "udp.h"
#include "net.h"
#include "shm.h"

extern void sched_sleep_ticks(uint32_t ticks);
extern void sched_task_exit(int exit_code);
extern int  sched_waitpid(int target_pid, int* status);

struct syscall_frame {
    uint32_t es, ds;
    uint32_t edi, esi, ebp, esp_dummy, ebx, edx, ecx, eax;
    uint32_t eip, cs, eflags, useresp, ss;
} __attribute__((packed));

#define KERNEL_BASE      0xC0000000U
#define USER_SPACE_START 0x08000000U  /* first 128 MB is identity-mapped kernel memory */
#define USER_STR_MAX     4096

static int validate_user_ptr(const void* ptr, uint32_t size) {
    uint32_t addr = (uint32_t)ptr;
    if (!ptr)                     return 0;
    if (addr < USER_SPACE_START)  return 0;
    if (addr >= KERNEL_BASE)      return 0;
    if (size == 0)                return 1;
    uint32_t end = addr + size;
    if (end < addr)               return 0;
    if (end > KERNEL_BASE)        return 0;
    return 1;
}

static int validate_user_str(const char* str) {
    uint32_t addr = (uint32_t)str;
    if (!str)                     return 0;
    if (addr < USER_SPACE_START)  return 0;
    if (addr >= KERNEL_BASE)      return 0;
    for (uint32_t i = 0; i < USER_STR_MAX; i++) {
        if (addr + i >= KERNEL_BASE) return 0;
        if (str[i] == '\0')          return 1;
    }
    return 0;
}

static void _make_abs(const char *path, char *abs, int abs_max) {
    int p = 0;
    if (path[0] != '/') {
        for (int i = 0; current_task->cwd[i] && p < abs_max - 2; i++)
            abs[p++] = current_task->cwd[i];
        if (p > 0 && abs[p-1] != '/') abs[p++] = '/';
    }
    for (int i = 0; path[i] && p < abs_max - 1; i++)
        abs[p++] = path[i];
    abs[p] = '\0';
}

static vfs_node_t* _resolve_path(const char* path) {
    if (!path || !current_task) return 0;
    char abs[512];
    _make_abs(path, abs, 512);
    return vfs_walk_path_follow(vfs_root, abs, 0);
}

static vfs_node_t* _resolve_parent_follow(const char* path,
                                           char* basename_out, int basename_max) {
    if (!path || !current_task) return 0;

    char abs[512];
    _make_abs(path, abs, 512);

    int last_slash = -1;
    for (int i = 0; abs[i]; i++)
        if (abs[i] == '/') last_slash = i;

    if (last_slash < 0) {
        int i = 0;
        while (path[i] && i < basename_max - 1) { basename_out[i] = path[i]; i++; }
        basename_out[i] = '\0';
        return vfs_walk_path_follow(vfs_root, current_task->cwd, 0);
    }

    const char *bn = abs + last_slash + 1;
    int i = 0;
    while (bn[i] && i < basename_max - 1) { basename_out[i] = bn[i]; i++; }
    basename_out[i] = '\0';

    if (last_slash == 0) return vfs_root;

    char parent_path[512];
    for (int j = 0; j < last_slash && j < 511; j++)
        parent_path[j] = abs[j];
    parent_path[last_slash] = '\0';

    return vfs_walk_path_follow(vfs_root, parent_path, 0);
}

static vfs_node_t* _resolve_parent(const char* path, char* basename_out, int basename_max) {
    return _resolve_parent_follow(path, basename_out, basename_max);
}

static int sys_print(char* msg) {
    if (!validate_user_str(msg)) return -1;
    kprint(msg);
    return 0;
}

static int sys_get_pid() {
    return (int)current_task->pid;
}

static int sys_getppid() {
    if (!current_task) return -1;
    return (int)current_task->parent_pid;
}

static int sys_getuid()  { return current_task ? (int)current_task->uid  : -1; }
static int sys_getgid()  { return current_task ? (int)current_task->gid  : -1; }
static int sys_geteuid() { return current_task ? (int)current_task->euid : -1; }
static int sys_getegid() { return current_task ? (int)current_task->egid : -1; }

static int sys_setuid(uint32_t uid) {
    if (!current_task) return -1;
    if (current_task->euid != 0 && uid != current_task->uid)
        return -1;
    current_task->uid  = uid;
    current_task->euid = uid;
    return 0;
}

static int sys_setgid(uint32_t gid) {
    if (!current_task) return -1;
    if (current_task->euid != 0 && gid != current_task->gid)
        return -1;
    current_task->gid  = gid;
    current_task->egid = gid;
    return 0;
}

static int sys_chmod(char *path, uint32_t mode) {
    if (!validate_user_str(path)) return -1;
    if (!current_task) return -1;

    vfs_node_t *node = _resolve_path(path);
    if (!node) return -1;

    if (current_task->euid != 0 && current_task->euid != node->uid)
        return -1;

    node->mode = mode & 0777;
    return 0;
}

static int sys_chown(char *path, uint32_t new_uid, uint32_t new_gid) {
    if (!validate_user_str(path)) return -1;
    if (!current_task) return -1;

    vfs_node_t *node = _resolve_path(path);
    if (!node) return -1;

    if (current_task->euid != 0) return -1;

    if (new_uid != (uint32_t)-1) node->uid = new_uid;
    if (new_gid != (uint32_t)-1) node->gid = new_gid;
    return 0;
}

struct lux_dirent {
    uint32_t d_ino;
    char     d_name[124];
};

static int sys_getdents(struct syscall_frame* regs) {
    int       fd    = (int)regs->ebx;
    char*     buf   = (char*)regs->ecx;
    uint32_t  count = regs->edx;

    if (!current_task) return -1;
    if (fd < 0 || fd >= MAX_FD) return -1;

    struct vfs_node* node = current_task->fd_table[fd];
    if (!node) return -1;
    if (node->type != VFS_DIRECTORY) return -1;

    uint32_t entry_size = sizeof(struct lux_dirent);
    if (!validate_user_ptr(buf, count)) return -1;
    if (count < entry_size) return -1;

    uint32_t written = 0;
    uint32_t index = current_task->fd_offset[fd];

    while (written + entry_size <= count) {
        struct vfs_dirent* de = readdir_vfs(node, index);
        if (!de) break;

        struct lux_dirent* out = (struct lux_dirent*)(buf + written);
        out->d_ino = de->inode;

        int i = 0;
        while (de->name[i] && i < 123) { out->d_name[i] = de->name[i]; i++; }
        out->d_name[i] = '\0';

        written += entry_size;
        index++;
    }

    current_task->fd_offset[fd] = index;
    return (int)written;
}

static int sys_rename(char* oldpath, char* newpath) {
    if (!validate_user_str(oldpath)) return -1;
    if (!validate_user_str(newpath)) return -1;
    if (!current_task) return -1;

    char old_base[128], new_base[128];
    vfs_node_t* old_parent = _resolve_parent(oldpath, old_base, 128);
    vfs_node_t* new_parent = _resolve_parent(newpath, new_base, 128);

    if (!old_parent || !new_parent) return -1;
    if (!old_base[0] || !new_base[0]) return -1;

    if (old_parent != new_parent) return -1;

    return rename_vfs(old_parent, old_base, new_base);
}

static int sys_open(char* name, int flags) {
    if (!validate_user_str(name)) return -1;
    if (!current_task) return -1;

    vfs_node_t* node = _resolve_path(name);
    if (!node) {
        kprint("[DBG] sys_open: not found: "); kprint(name); kprint("\n");
        return -1;
    }

    {
        uint32_t need = 0;
        uint32_t acc = (uint32_t)flags & 3;   
        if (acc == 0 || acc == 2) need |= VFS_PERM_READ;
        if (acc == 1 || acc == 2) need |= VFS_PERM_WRITE;
        if (vfs_check_perm(node, need) < 0) return -1;
    }

    for (int i = 3; i < MAX_FD; i++) {
        if (!current_task->fd_table[i]) {
            current_task->fd_table[i] = node;
            current_task->fd_offset[i] = 0;
            current_task->fd_flags[i]   = (uint32_t)flags;
            current_task->fd_cloexec[i] = 0;
            /* Increment refcount so unlink-while-open defers free until close. */
            open_vfs(node);
            kprint("[DBG] sys_open: fd=");
            char tmp[16]; itoa(i, tmp); kprint(tmp);
            kprint(" path="); kprint(name); kprint("\n");
            return i;
        }
    }
    return -1;
}

static int sys_read(int fd, char* buf, unsigned int size) {
    if (!size)                         return -1;
    if (!validate_user_ptr(buf, size)) return -1;
    if (!current_task)                 return -1;
    if (fd < 0 || fd >= MAX_FD)        return -1;

    struct vfs_node* node = current_task->fd_table[fd];
    if (!node) return -1;
    int ret = read_vfs(node, current_task->fd_offset[fd], size, buf);
    if (ret > 0)
        current_task->fd_offset[fd] += (uint32_t)ret;
    return ret;
}

static int sys_write(int fd, char* buf, unsigned int size) {
    if (!size)                         return -1;
    if (!validate_user_ptr(buf, size)) return -1;
    if (!current_task)                 return -1;
    if (fd < 0 || fd >= MAX_FD)        return -1;
    struct vfs_node* node = current_task->fd_table[fd];
    if (!node) return -1;
    int ret = write_vfs(node, current_task->fd_offset[fd], size, buf);
    if (ret > 0)
        current_task->fd_offset[fd] += (uint32_t)ret;
    return ret;
}

static int sys_close(int fd) {
    if (!current_task)          return -1;
    if (fd < 0 || fd >= MAX_FD) return -1;
    struct vfs_node* node = current_task->fd_table[fd];
    if (!node) return -1;
    current_task->fd_table[fd]   = 0;
    current_task->fd_offset[fd]  = 0;
    current_task->fd_flags[fd]   = 0;
    current_task->fd_cloexec[fd] = 0;
    close_vfs(node);
    return 0;
}

static int sys_create(char* name) {
    if (!validate_user_str(name)) return -1;
    if (!current_task) return -1;

    char basename[128];
    vfs_node_t* parent = _resolve_parent(name, basename, 128);
    if (!parent) {
        kprint("[DBG] sys_create: parent not found for: "); kprint(name); kprint("\n");
        return -1;
    }
    if (!basename[0]) {
        kprint("[DBG] sys_create: empty basename\n");
        return -1;
    }

    kprint("[DBG] sys_create: parent="); kprint(parent->name);
    kprint(" basename="); kprint(basename); kprint("\n");

    return create_vfs(parent, basename);
}

static int sys_delete(char* name) {
    if (!validate_user_str(name)) return -1;
    if (!current_task) return -1;

    char basename[128];
    vfs_node_t* parent = _resolve_parent(name, basename, 128);
    if (!parent) {
        kprint("[DBG] sys_delete: parent not found for: "); kprint(name); kprint("\n");
        return -1;
    }
    if (!basename[0]) return -1;

    kprint("[DBG] sys_delete: parent="); kprint(parent->name);
    kprint(" basename="); kprint(basename); kprint("\n");

    return delete_vfs(parent, basename);
}

static int sys_mkdir(char* pathname) {
    if (!validate_user_str(pathname)) return -1;
    if (!current_task) return -1;

    char basename[128];
    vfs_node_t* parent = _resolve_parent(pathname, basename, 128);
    if (!parent) {
        kprint("[DBG] sys_mkdir: parent not found for: "); kprint(pathname); kprint("\n");
        return -1;
    }
    if (!basename[0]) {
        kprint("[DBG] sys_mkdir: empty basename\n");
        return -1;
    }

    if (vfs_check_perm(parent, VFS_PERM_WRITE | VFS_PERM_EXEC) < 0) return -1;

    kprint("[DBG] sys_mkdir: parent="); kprint(parent->name);
    kprint(" basename="); kprint(basename); kprint("\n");

    return mkdir_vfs(parent, basename);
}

static int sys_rmdir(char* pathname) {
    if (!validate_user_str(pathname)) return -1;
    if (!current_task) return -1;

    char basename[128];
    vfs_node_t* parent = _resolve_parent(pathname, basename, 128);
    if (!parent) {
        kprint("[DBG] sys_rmdir: parent not found for: "); kprint(pathname); kprint("\n");
        return -1;
    }
    if (!basename[0]) {
        kprint("[DBG] sys_rmdir: empty basename\n");
        return -1;
    }
    if (vfs_check_perm(parent, VFS_PERM_WRITE | VFS_PERM_EXEC) < 0) return -1;

    kprint("[DBG] sys_rmdir: parent="); kprint(parent->name);
    kprint(" basename="); kprint(basename); kprint("\n");

    return rmdir_vfs(parent, basename);
}

static int sys_exit(struct syscall_frame* regs) {
    if (!current_task) return -1;

    if (terminal_fg_pid == current_task->pid)
        terminal_fg_pid = 0;

    sched_task_exit((int)regs->ebx);

    /* Zombie: spin until timer interrupt preempts us.
       Scheduler sees Zombie, wakes parent, never re-enqueues us. */
    for (;;)
        __asm__ volatile ("hlt");
}

static int sys_fork(struct syscall_frame* regs) {
    char hbuf[12];
    kprint("[SYSFORK] eip=0x");
    hex_to_ascii(regs->eip, hbuf); kprint(hbuf);
    kprint(" uesp=0x");
    hex_to_ascii(regs->useresp, hbuf); kprint(hbuf);
    kprint(" cs=0x");
    hex_to_ascii(regs->cs, hbuf); kprint(hbuf);
    kprint(" ss=0x");
    hex_to_ascii(regs->ss, hbuf); kprint(hbuf);
    kprint(" eflags=0x");
    hex_to_ascii(regs->eflags, hbuf); kprint(hbuf);
    kprint("\n");

    struct context_frame cf;
    cf.es        = regs->es;        cf.ds       = regs->ds;
    cf.edi       = regs->edi;       cf.esi      = regs->esi;
    cf.ebp       = regs->ebp;       cf.esp_dummy= regs->esp_dummy;
    cf.ebx       = regs->ebx;       cf.edx      = regs->edx;
    cf.ecx       = regs->ecx;       cf.eax      = regs->eax;
    cf.eip       = regs->eip;       cf.cs       = regs->cs;
    cf.eflags    = regs->eflags;
    cf.useresp   = regs->useresp;   cf.ss       = regs->ss;
    struct task_struct* child = task_fork(&cf);
    if (!child) {
        kprint("[SYSFORK] task_fork returned NULL!\n");
        return -1;
    }
    kprint("[SYSFORK] ok pid=");
    itoa((int)child->pid, hbuf); kprint(hbuf);
    kprint("\n");
    return (int)child->pid;
}

#define EXEC_VALIDATE_MAX 256

static int sys_exec(struct syscall_frame* regs) {
    char* path = (char*)regs->ebx;
    if (!validate_user_str(path)) return -1;

    char** argv = (char**)regs->ecx;
    char** envp = (char**)regs->edx;

    if (argv) {
        if (!validate_user_ptr(argv, sizeof(char*))) return -1;
        for (int i = 0; i < EXEC_VALIDATE_MAX; i++) {
            if ((uint32_t)&argv[i] < USER_SPACE_START || (uint32_t)&argv[i] >= KERNEL_BASE) return -1;
            if (!argv[i]) break;
            if (!validate_user_str(argv[i])) return -1;
        }
    }

    if (envp) {
        if (!validate_user_ptr(envp, sizeof(char*))) return -1;
        for (int i = 0; i < EXEC_VALIDATE_MAX; i++) {
            if ((uint32_t)&envp[i] < USER_SPACE_START || (uint32_t)&envp[i] >= KERNEL_BASE) return -1;
            if (!envp[i]) break;
            if (!validate_user_str(envp[i])) return -1;
        }
    }

    {
        vfs_node_t *exec_node = _resolve_path(path);
        if (exec_node && vfs_check_perm(exec_node, VFS_PERM_EXEC) < 0)
            return -1;
    }

    struct context_frame cf;
    cf.eip     = regs->eip;
    cf.cs      = regs->cs;
    cf.eflags  = regs->eflags;
    cf.useresp = regs->useresp;
    cf.ss      = regs->ss;
    return task_exec(path, argv, envp, &cf);
}

static int sys_kill(uint32_t pid) {
    task_kill(pid);
    return 0;
}

static int sys_signal(uint32_t pid, uint32_t signal) {
    if (!pid) return -1;
    task_signal(pid, signal);
    return 0;
}

static int sys_mmap(struct syscall_frame* regs) {
    mmap_args_t* args = (mmap_args_t*)regs->ebx;
    if (!validate_user_ptr(args, sizeof(mmap_args_t))) return (int)MAP_FAILED;
    if (!current_task) return (int)MAP_FAILED;
    void* result = do_mmap(
        current_task->page_directory,
        &current_task->mmap_table,
        args->addr,
        args->length,
        args->prot,
        args->flags,
        args->fd,
        args->offset
    );
    return (int)result;
}

static int sys_munmap(struct syscall_frame* regs) {
    uint32_t addr   = regs->ebx;
    uint32_t length = regs->ecx;
    if (!current_task) return -1;
    return do_munmap(
        current_task->page_directory,
        &current_task->mmap_table,
        addr, length
    );
}

static int sys_mprotect(struct syscall_frame* regs) {
    uint32_t addr   = regs->ebx;
    uint32_t length = regs->ecx;
    int      prot   = (int)regs->edx;
    if (!current_task) return -1;
    return do_mprotect(
        current_task->page_directory,
        &current_task->mmap_table,
        addr, length, prot
    );
}

static int sys_pipe(struct syscall_frame* regs) {
    int* user_fds = (int*)regs->ebx;
    if (!validate_user_ptr(user_fds, sizeof(int) * 2)) return -1;
    if (!current_task) return -1;

    struct vfs_node* pipefd[2];
    if (pipe_create(pipefd, 0) != 0) return -1;

    int rfd = -1, wfd = -1;
    for (int i = 3; i < MAX_FD && (rfd < 0 || wfd < 0); i++) {
        if (!current_task->fd_table[i]) {
            if (rfd < 0) rfd = i;
            else         wfd = i;
        }
    }

    if (rfd < 0 || wfd < 0) {
        close_vfs(pipefd[0]);
        close_vfs(pipefd[1]);
        return -1;
    }

    /* No open_vfs here: pipe_create/_make_node already incremented
     * pipe_t.ref_count once per node, which balances the close_vfs in
     * sys_close.  Unlike regular files, pipe nodes are not in any
     * directory tree so there is no separate "dir-link" refcount. */
    current_task->fd_table[rfd]   = pipefd[0];
    current_task->fd_table[wfd]   = pipefd[1];
    current_task->fd_offset[rfd]  = 0;
    current_task->fd_offset[wfd]  = 0;
    current_task->fd_flags[rfd]   = 0;
    current_task->fd_flags[wfd]   = 1;
    current_task->fd_cloexec[rfd] = 0;
    current_task->fd_cloexec[wfd] = 0;

    user_fds[0] = rfd;
    user_fds[1] = wfd;
    return 0;
}

static int sys_dup2(struct syscall_frame* regs) {
    int oldfd = (int)regs->ebx;
    int newfd = (int)regs->ecx;

    if (!current_task) return -1;
    if (oldfd < 0 || oldfd >= MAX_FD) return -1;
    if (newfd < 0 || newfd >= MAX_FD) return -1;

    /* POSIX: dup2(fd, fd) is a no-op that returns fd. */
    if (oldfd == newfd) return newfd;

    struct vfs_node* node = current_task->fd_table[oldfd];
    if (!node) return -1;

    /* Increment first so the node is never transiently unreferenced even if
     * newfd happens to alias the same underlying object (e.g. after a previous
     * dup2). This prevents use-after-free when old_newnode == node. */
    open_vfs(node);

    struct vfs_node* old_newnode = current_task->fd_table[newfd];
    current_task->fd_table[newfd]   = node;
    current_task->fd_offset[newfd]  = current_task->fd_offset[oldfd];
    current_task->fd_flags[newfd]   = current_task->fd_flags[oldfd];
    current_task->fd_cloexec[newfd] = 0;

    /* Decrement the evicted slot only after the new reference is established. */
    if (old_newnode)
        close_vfs(old_newnode);

    return newfd;
}

static int sys_sigaction(struct syscall_frame* regs) {
    uint32_t signum  = regs->ebx;
    uint32_t handler = regs->ecx;

    if (!current_task) return -1;

    if (handler > SIG_IGN && (handler < USER_SPACE_START || handler >= KERNEL_BASE)) return -1;

    return task_sigaction(current_task, signum, handler);
}

static int sys_sigreturn(struct syscall_frame* regs) {
    if (!current_task || current_task->is_kernel) return -1;

    uint32_t user_esp = regs->useresp;
    if (user_esp < USER_SPACE_START || user_esp >= KERNEL_BASE) return -1;

    uint32_t* pd  = current_task->page_directory;
    uint32_t  pdi = PD_INDEX(user_esp);
    uint32_t  pti = PT_INDEX(user_esp);
    if (!pd || !(pd[pdi] & PAGE_PRESENT)) return -1;
    uint32_t* pt = (uint32_t*)(pd[pdi] & ~0xFFFu);
    if (!(pt[pti] & PAGE_PRESENT)) return -1;

    uint32_t phys_page = pt[pti] & ~0xFFFu;
    uint32_t page_off  = user_esp & 0xFFFu;
    if (page_off + sizeof(signal_frame_t) > PAGE_SIZE) return -1;

    signal_frame_t* frame = (signal_frame_t*)(phys_page + page_off);

    regs->eax     = frame->eax;
    regs->ecx     = frame->ecx;
    regs->edx     = frame->edx;
    regs->ebx     = frame->ebx;
    regs->ebp     = frame->ebp;
    regs->esi     = frame->esi;
    regs->edi     = frame->edi;
    regs->eip     = frame->eip;
    regs->eflags  = frame->eflags;
    regs->useresp = frame->esp;

    return 0;
}

extern uint32_t timer_ticks_get(void);

static int sys_sigprocmask(struct syscall_frame* regs) {
    int      how    = (int)regs->ebx;
    uint32_t* set    = (uint32_t*)regs->ecx;
    uint32_t* oldset = (uint32_t*)regs->edx;

    if (!current_task) return -1;
    if (set    && !validate_user_ptr(set,    sizeof(uint32_t))) return -1;
    if (oldset && !validate_user_ptr(oldset, sizeof(uint32_t))) return -1;

    return task_sigprocmask(current_task, how, set, oldset);
}

static int sys_sigpending(struct syscall_frame* regs) {
    uint32_t* set = (uint32_t*)regs->ebx;

    if (!current_task) return -1;
    if (!validate_user_ptr(set, sizeof(uint32_t))) return -1;

    *set = current_task->pending_signals & current_task->signal_mask;
    return 0;
}

static int sys_sigsuspend(struct syscall_frame* regs) {
    uint32_t* mask = (uint32_t*)regs->ebx;

    if (!current_task || current_task->is_kernel) return -1;
    if (!validate_user_ptr(mask, sizeof(uint32_t))) return -1;

    current_task->saved_signal_mask = current_task->signal_mask;
    current_task->signal_mask = *mask & ~SIG_UNCATCHABLE;
    current_task->in_sigsuspend = 1;
    current_task->state = TASK_SLEEPING;

    schedule();
    return -1;
}

#define TIMER_HZ_SIGNALS 100   

static int sys_alarm(struct syscall_frame* regs) {
    uint32_t seconds = (uint32_t)regs->ebx;

    if (!current_task) return -1;

    uint32_t now = timer_ticks_get();
    uint32_t remaining = 0;

    if (current_task->alarm_ticks) {
        uint32_t left_ticks = (current_task->alarm_ticks > now)
                              ? (current_task->alarm_ticks - now) : 0;
        remaining = (left_ticks + TIMER_HZ_SIGNALS - 1) / TIMER_HZ_SIGNALS;
    }

    if (seconds == 0) {
        current_task->alarm_ticks = 0;
    } else {
        current_task->alarm_ticks = now + seconds * TIMER_HZ_SIGNALS;
    }

    return (int)remaining;
}

struct itimerval_k {
    struct { long tv_sec; long tv_usec; } it_interval;
    struct { long tv_sec; long tv_usec; } it_value;
};

static int sys_setitimer(struct syscall_frame* regs) {
    int which                    = (int)regs->ebx;
    struct itimerval_k* newval   = (struct itimerval_k*)regs->ecx;
    struct itimerval_k* oldval   = (struct itimerval_k*)regs->edx;

    if (!current_task) return -1;
    if (which != 0) return -1;
    if (newval && !validate_user_ptr(newval, sizeof(struct itimerval_k))) return -1;
    if (oldval && !validate_user_ptr(oldval, sizeof(struct itimerval_k))) return -1;

    uint32_t now = timer_ticks_get();

    if (oldval) {
        if (current_task->itimer_value && current_task->itimer_value > now) {
            uint32_t left = current_task->itimer_value - now;
            oldval->it_value.tv_sec  = left / TIMER_HZ_SIGNALS;
            oldval->it_value.tv_usec = (long)((left % TIMER_HZ_SIGNALS) *
                                               (1000000 / TIMER_HZ_SIGNALS));
        } else {
            oldval->it_value.tv_sec  = 0;
            oldval->it_value.tv_usec = 0;
        }
        oldval->it_interval.tv_sec  = current_task->itimer_interval / TIMER_HZ_SIGNALS;
        oldval->it_interval.tv_usec = (long)((current_task->itimer_interval % TIMER_HZ_SIGNALS) *
                                              (1000000 / TIMER_HZ_SIGNALS));
    }

    if (newval) {
        uint32_t val_ticks = (uint32_t)(newval->it_value.tv_sec * TIMER_HZ_SIGNALS) +
                             (uint32_t)((newval->it_value.tv_usec * TIMER_HZ_SIGNALS) / 1000000);
        uint32_t int_ticks = (uint32_t)(newval->it_interval.tv_sec * TIMER_HZ_SIGNALS) +
                             (uint32_t)((newval->it_interval.tv_usec * TIMER_HZ_SIGNALS) / 1000000);

        if (val_ticks == 0) {
            current_task->itimer_value    = 0;
            current_task->itimer_interval = 0;
        } else {
            current_task->itimer_value    = now + val_ticks;
            current_task->itimer_interval = int_ticks;
        }
    }

    return 0;
}

//public api
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

static int sys_lseek(struct syscall_frame* regs) {
    int fd          = (int)regs->ebx;
    int offset      = (int)regs->ecx;
    int whence      = (int)regs->edx;

    if (!current_task) return -1;
    if (fd < 0 || fd >= MAX_FD) return -1;
    struct vfs_node* node = current_task->fd_table[fd];
    if (!node) return -1;

    uint32_t new_off;
    switch (whence) {
        case SEEK_SET:
            if (offset < 0) return -1;
            new_off = (uint32_t)offset;
            break;
        case SEEK_CUR: {
            int cur = (int)current_task->fd_offset[fd] + offset;
            if (cur < 0) return -1;
            new_off = (uint32_t)cur;
            break;
        }
        case SEEK_END: {
            int end = (int)node->size + offset;
            if (end < 0) return -1;
            new_off = (uint32_t)end;
            break;
        }
        default:
            return -1;
    }
    current_task->fd_offset[fd] = new_off;
    return (int)new_off;
}

static int sys_waitpid(struct syscall_frame* regs) {
    int target_pid = (int)regs->ebx;
    int* status    = (int*)regs->ecx;

    if (!current_task) return -1;
    if (status && !validate_user_ptr(status, sizeof(int))) return -1;

    return sched_waitpid(target_pid, status);
}

static int sys_sleep(struct syscall_frame* regs) {
    uint32_t ms = regs->ebx;
    if (!current_task) return -1;
    if (ms == 0) return 0;

    uint32_t ticks = (ms + 9) / 10;
    sched_sleep_ticks(ticks);
    return 0;
}

static int sys_brk(struct syscall_frame* regs) {
    uint32_t new_brk = regs->ebx;
    if (!current_task) return -1;

    if (new_brk == 0)
        return (int)current_task->brk_current;

    if (new_brk < current_task->brk_start)
        return -1;

    if (new_brk - current_task->brk_start > 16 * 1024 * 1024)
        return -1;

    uint32_t old_end = (current_task->brk_current + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    uint32_t new_end = (new_brk + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);

    if (new_end > old_end) {
        for (uint32_t va = old_end; va < new_end; va += PAGE_SIZE) {
            void* phys = kalloc();
            if (!phys) return -1;
            uint8_t* p = (uint8_t*)phys;
            for (int i = 0; i < (int)PAGE_SIZE; i++) p[i] = 0;
            vmm_map(current_task->page_directory, va, (uint32_t)phys,
                    PAGE_USER | PAGE_RW | PAGE_PRESENT);
            proc_tracker_add(&current_task->mm, phys);
        }
    }

    current_task->brk_current = new_brk;
    return (int)new_brk;
}

static int sys_getcwd(struct syscall_frame* regs) {
    char*    buf  = (char*)regs->ebx;
    uint32_t size = regs->ecx;

    if (!current_task) return -1;
    if (!buf || size == 0) return -1;
    if (!validate_user_ptr(buf, size)) return -1;

    uint32_t len = 0;
    while (current_task->cwd[len]) len++;
    len++;

    if (len > size) return -1;

    for (uint32_t i = 0; i < len; i++)
        buf[i] = current_task->cwd[i];

    kprint("[DBG] sys_getcwd: pid=");
    char tmp[16]; itoa(current_task->pid, tmp); kprint(tmp);
    kprint(" cwd="); kprint(current_task->cwd); kprint("\n");

    return (int)buf;
}

static int sys_chdir(struct syscall_frame* regs) {
    char* path = (char*)regs->ebx;

    if (!current_task) return -1;
    if (!validate_user_str(path)) return -1;

    char abs[256];
    if (path[0] == '/') {
        int i = 0;
        while (path[i] && i < 255) { abs[i] = path[i]; i++; }
        abs[i] = '\0';
    } else {
        int p = 0;
        for (int i = 0; current_task->cwd[i] && p < 254; i++)
            abs[p++] = current_task->cwd[i];
        if (p > 0 && abs[p-1] != '/') abs[p++] = '/';
        for (int i = 0; path[i] && p < 255; i++)
            abs[p++] = path[i];
        abs[p] = '\0';
    }

    int segs_start[64], segs_len[64];
    int nseg = 0;
    const char* s = abs;
    while (*s) {
        while (*s == '/') s++;
        if (!*s) break;
        const char* seg = s;
        int slen = 0;
        while (*s && *s != '/') { s++; slen++; }
        if (slen == 1 && seg[0] == '.')
            continue;
        if (slen == 2 && seg[0] == '.' && seg[1] == '.') {
            if (nseg > 0) nseg--;
            continue;
        }
        segs_start[nseg] = (int)(seg - abs);
        segs_len[nseg]   = slen;
        nseg++;
        if (nseg >= 64) break;
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

    vfs_node_t* node = vfs_walk_path(vfs_root, norm);
    if (!node) {
        kprint("[DBG] sys_chdir: not found: "); kprint(norm); kprint("\n");
        return -1;
    }
    if (node->type != VFS_DIRECTORY) {
        kprint("[DBG] sys_chdir: not a dir: "); kprint(norm); kprint("\n");
        return -1;
    }

    int i = 0;
    while (norm[i] && i < 255) { current_task->cwd[i] = norm[i]; i++; }
    current_task->cwd[i] = '\0';

    kprint("[DBG] sys_chdir: pid=");
    char tmp[16]; itoa(current_task->pid, tmp); kprint(tmp);
    kprint(" -> "); kprint(current_task->cwd); kprint("\n");

    return 0;
}

static uint32_t _vfs_type_to_mode(uint32_t type) {
    switch (type) {
    case VFS_FILE:        return 0x8000;
    case VFS_DIRECTORY:   return 0x4000;
    case VFS_CHARDEVICE:  return 0x2000;
    case VFS_BLOCKDEVICE: return 0x6000;
    case VFS_PIPE:        return 0x1000;
    default:              return 0;
    }
}

static void _fill_stat(struct vfs_node* node, uint32_t* ubuf) {
    ubuf[0] = node->inode;
    ubuf[1] = _vfs_type_to_mode(node->type);
    ubuf[2] = node->size;
    ubuf[3] = node->type;
}

static int sys_stat(struct syscall_frame* regs) {
    char*     path = (char*)regs->ebx;
    uint32_t* ubuf = (uint32_t*)regs->ecx;

    if (!current_task) return -1;
    if (!validate_user_str(path)) return -1;
    if (!validate_user_ptr(ubuf, 16)) return -1;

    struct vfs_node* node = vfs_walk_path(vfs_root, path);
    if (!node) {
        kprint("[DBG] sys_stat: not found: "); kprint(path); kprint("\n");
        return -1;
    }

    _fill_stat(node, ubuf);

    kprint("[DBG] sys_stat: "); kprint(path);
    kprint(" type=");
    char tmp[16]; itoa((int)node->type, tmp); kprint(tmp);
    kprint(" size="); itoa((int)node->size, tmp); kprint(tmp);
    kprint("\n");

    return 0;
}

static int sys_fstat(struct syscall_frame* regs) {
    int       fd   = (int)regs->ebx;
    uint32_t* ubuf = (uint32_t*)regs->ecx;

    if (!current_task) return -1;
    if (fd < 0 || fd >= MAX_FD) return -1;
    if (!validate_user_ptr(ubuf, 16)) return -1;

    struct vfs_node* node = current_task->fd_table[fd];
    if (!node) {
        kprint("[DBG] sys_fstat: bad fd=");
        char tmp[16]; itoa(fd, tmp); kprint(tmp); kprint("\n");
        return -1;
    }

    _fill_stat(node, ubuf);

    kprint("[DBG] sys_fstat: fd=");
    char tmp[16]; itoa(fd, tmp); kprint(tmp);
    kprint(" type="); itoa((int)node->type, tmp); kprint(tmp);
    kprint(" size="); itoa((int)node->size, tmp); kprint(tmp);
    kprint("\n");

    return 0;
}

static int sys_ioctl(struct syscall_frame* regs) {
    int       fd  = (int)regs->ebx;
    uint32_t  cmd = regs->ecx;
    void*     arg = (void*)regs->edx;

    if (!current_task) return -1;
    if (fd < 0 || fd >= MAX_FD) return -1;

    if (cmd == TIOCGWINSZ) {
        if (!arg || !validate_user_ptr(arg, sizeof(struct winsize))) return -1;
        struct winsize* ws = (struct winsize*)arg;
        ws->ws_row    = terminal_winsize.ws_row;
        ws->ws_col    = terminal_winsize.ws_col;
        ws->ws_xpixel = terminal_winsize.ws_xpixel;
        ws->ws_ypixel = terminal_winsize.ws_ypixel;
        return 0;
    }

    if (cmd == TIOCSWINSZ) {
        if (!arg || !validate_user_ptr(arg, sizeof(struct winsize))) return -1;
        struct winsize* ws = (struct winsize*)arg;
        terminal_winsize.ws_row    = ws->ws_row;
        terminal_winsize.ws_col    = ws->ws_col;
        terminal_winsize.ws_xpixel = ws->ws_xpixel;
        terminal_winsize.ws_ypixel = ws->ws_ypixel;
        if (terminal_fg_pid)
            task_signal(terminal_fg_pid, SIGWINCH);
        return 0;
    }

    struct vfs_node* node = current_task->fd_table[fd];
    if (!node) return -1;

    if (arg && !validate_user_ptr(arg, 1)) return -1;

    return ioctl_vfs(node, cmd, arg);
}

#define SETFL_MASK  (0x0800 | 0x0400)

static int sys_fcntl(int fd, int cmd, int arg) {
    if (!current_task)          return -1;
    if (fd < 0 || fd >= MAX_FD) return -1;

    struct vfs_node* node = current_task->fd_table[fd];
    if (!node) return -1;

    switch (cmd) {

    case 0: { 
        if (arg < 0 || arg >= MAX_FD) return -1;
        for (int i = arg; i < MAX_FD; i++) {
            if (!current_task->fd_table[i]) {
                current_task->fd_table[i]   = node;
                current_task->fd_offset[i]  = current_task->fd_offset[fd];
                current_task->fd_flags[i]   = current_task->fd_flags[fd];
                current_task->fd_cloexec[i] = 0; 
                open_vfs(node);
                return i;
            }
        }
        return -1;
    }

    case 1:
        return (int)current_task->fd_cloexec[fd]; 

    case 2: 
        current_task->fd_cloexec[fd] = (arg & 1) ? 1 : 0;
        return 0;

    case 3: 
        return (int)current_task->fd_flags[fd];

    case 4: { 
        uint32_t new_flags = (current_task->fd_flags[fd] & ~(uint32_t)SETFL_MASK)
                           | ((uint32_t)arg & SETFL_MASK);
        current_task->fd_flags[fd] = new_flags;

        if (node->type == VFS_PIPE && node->priv) {
            pipe_t* p = (pipe_t*)node->priv;
            if (new_flags & 0x0800)
                p->flags |=  O_NONBLOCK;
            else
                p->flags &= ~O_NONBLOCK;
        }
        return 0;
    }

    default:
        return -1;
    }
}

#define TIMER_HZ 100

struct timeval {
    long tv_sec;
    long tv_usec;
};

struct timespec {
    long tv_sec;
    long tv_nsec;
};

#define CLOCK_REALTIME  0
#define CLOCK_MONOTONIC 1

static int sys_gettimeofday(struct syscall_frame* regs) {
    struct timeval* tv = (struct timeval*)regs->ebx;

    if (!tv) return 0;
    if (!validate_user_ptr(tv, sizeof(struct timeval))) return -1;

    uint32_t ticks = timer_ticks_get();
    tv->tv_sec  = (long)(ticks / TIMER_HZ);
    tv->tv_usec = (long)((ticks % TIMER_HZ) * (1000000 / TIMER_HZ));
    return 0;
}

static int sys_clock_gettime(struct syscall_frame* regs) {
    int clkid             = (int)regs->ebx;
    struct timespec* tp   = (struct timespec*)regs->ecx;

    if (!tp) return -1;
    if (!validate_user_ptr(tp, sizeof(struct timespec))) return -1;
    if (clkid != CLOCK_REALTIME && clkid != CLOCK_MONOTONIC) return -1;

    uint32_t ticks = timer_ticks_get();
    tp->tv_sec  = (long)(ticks / TIMER_HZ);
    tp->tv_nsec = (long)((ticks % TIMER_HZ) * (1000000000 / TIMER_HZ));
    return 0;
}

static int sys_nanosleep(struct syscall_frame* regs) {
    struct timespec* req = (struct timespec*)regs->ebx;
    struct timespec* rem = (struct timespec*)regs->ecx;

    if (!req) return -1;
    if (!validate_user_ptr(req, sizeof(struct timespec))) return -1;
    if (rem && !validate_user_ptr(rem, sizeof(struct timespec))) return -1;

    if (req->tv_sec < 0 || req->tv_nsec < 0 || req->tv_nsec >= 1000000000)
        return -1;

    if (!current_task) return -1;

    uint32_t ms   = (uint32_t)(req->tv_sec * 1000) +
                    (uint32_t)((req->tv_nsec + 999999) / 1000000);
    uint32_t ticks = (ms + (1000 / TIMER_HZ) - 1) / (1000 / TIMER_HZ);

    if (ticks == 0) {
        if (rem) { rem->tv_sec = 0; rem->tv_nsec = 0; }
        return 0;
    }

    sched_sleep_ticks(ticks);

    if (rem) { rem->tv_sec = 0; rem->tv_nsec = 0; }
    return 0;
}

static int alloc_fd(vfs_node_t *node) {
    for (int i = 3; i < MAX_FD; i++) {
        if (!current_task->fd_table[i]) {
            current_task->fd_table[i]   = node;
            current_task->fd_offset[i]  = 0;
            current_task->fd_flags[i]   = 0;
            current_task->fd_cloexec[i] = 0;
            open_vfs(node);
            return i;
        }
    }
    return -1;
}

static int sys_socket(struct syscall_frame *regs) {
    int domain   = (int)regs->ebx;
    int type     = (int)regs->ecx;
    int protocol = (int)regs->edx;
    if (!current_task) return -1;

    vfs_node_t *node = ksock_create(domain, type, protocol);
    if (!node) return -1;

    int fd = alloc_fd(node);
    if (fd < 0) {
        close_vfs(node);
        return -1;
    }
    return fd;
}

static int sys_bind(struct syscall_frame *regs) {
    int fd = (int)regs->ebx;
    struct sockaddr_in *addr = (struct sockaddr_in *)regs->ecx;

    if (!current_task) return -1;
    if (fd < 0 || fd >= MAX_FD) return -1;
    if (!validate_user_ptr(addr, sizeof(struct sockaddr_in))) return -1;

    vfs_node_t *node = current_task->fd_table[fd];
    if (!node || node->type != VFS_SOCKET) return -1;

    ksock_t *ks = ksock_from_node(node);
    if (!ks) return -1;

    uint16_t port = ntohs(addr->sin_port);

    if (ks->kind == KS_TCP) {
        tcp_socket_t *s = 0;
        if (ks->proto_idx >= 0 && ks->proto_idx < TCP_MAX_SOCKETS)
            s = &tcp_sockets[ks->proto_idx];
        if (!s) return -1;
        s->local_port = port;
        s->local_ip   = htonl(MY_IP);
        return 0;
    }

    if (ks->kind == KS_UDP) {
        udp_sock_t *s = udp_sock_find_by_port(port);
        if (s && s != &udp_socks[ks->proto_idx] && !ks->so_reuseaddr) return -1;
        udp_socks[ks->proto_idx].local_port = port;
        udp_socks[ks->proto_idx].local_ip   = ntohl(addr->sin_addr);
        return 0;
    }

    return -1;
}

static int sys_connect(struct syscall_frame *regs) {
    int fd = (int)regs->ebx;
    struct sockaddr_in *addr = (struct sockaddr_in *)regs->ecx;

    if (!current_task) return -1;
    if (fd < 0 || fd >= MAX_FD) return -1;
    if (!validate_user_ptr(addr, sizeof(struct sockaddr_in))) return -1;

    vfs_node_t *node = current_task->fd_table[fd];
    if (!node || node->type != VFS_SOCKET) return -1;

    ksock_t *ks = ksock_from_node(node);
    if (!ks || ks->kind != KS_TCP) return -1;

    uint32_t dst_ip   = addr->sin_addr;         
    uint16_t dst_port = ntohs(addr->sin_port);
    return tcp_connect(ks->proto_idx, dst_ip, dst_port);
}

static int sys_listen(struct syscall_frame *regs) {
    int fd      = (int)regs->ebx;

    if (!current_task) return -1;
    if (fd < 0 || fd >= MAX_FD) return -1;

    vfs_node_t *node = current_task->fd_table[fd];
    if (!node || node->type != VFS_SOCKET) return -1;

    ksock_t *ks = ksock_from_node(node);
    if (!ks || ks->kind != KS_TCP) return -1;

    extern tcp_socket_t tcp_sockets[];
    tcp_socket_t *s = &tcp_sockets[ks->proto_idx];
    return tcp_listen(ks->proto_idx, s->local_port);
}

static int sys_accept(struct syscall_frame *regs) {
    int fd = (int)regs->ebx;
    struct sockaddr_in *peer_addr    = (struct sockaddr_in *)regs->ecx;
    uint32_t           *peer_addrlen = (uint32_t *)regs->edx;

    if (!current_task) return -1;
    if (fd < 0 || fd >= MAX_FD) return -1;
    if (peer_addr && !validate_user_ptr(peer_addr, sizeof(struct sockaddr_in)))
        return -1;
    if (peer_addrlen && !validate_user_ptr(peer_addrlen, sizeof(uint32_t)))
        return -1;

    vfs_node_t *listen_node = current_task->fd_table[fd];
    if (!listen_node || listen_node->type != VFS_SOCKET) return -1;

    vfs_node_t *conn_node = ksock_tcp_accept(listen_node, peer_addr);
    if (!conn_node) return -1;  

    if (peer_addrlen) *peer_addrlen = sizeof(struct sockaddr_in);

    int new_fd = alloc_fd(conn_node);
    if (new_fd < 0) {
        close_vfs(conn_node);
        return -1;
    }
    return new_fd;
}

static int sys_send(struct syscall_frame *regs) {
    int      fd   = (int)regs->ebx;
    char    *buf  = (char *)regs->ecx;
    uint32_t len  = regs->edx;

    if (!current_task) return -1;
    if (fd < 0 || fd >= MAX_FD) return -1;
    if (!validate_user_ptr(buf, len)) return -1;

    vfs_node_t *node = current_task->fd_table[fd];
    if (!node || node->type != VFS_SOCKET) return -1;

    return write_vfs(node, 0, len, buf);
}

static int sys_recv(struct syscall_frame *regs) {
    int      fd   = (int)regs->ebx;
    char    *buf  = (char *)regs->ecx;
    uint32_t len  = regs->edx;

    if (!current_task) return -1;
    if (fd < 0 || fd >= MAX_FD) return -1;
    if (!validate_user_ptr(buf, len)) return -1;

    vfs_node_t *node = current_task->fd_table[fd];
    if (!node || node->type != VFS_SOCKET) return -1;

    return read_vfs(node, 0, len, buf);
}

static int sys_sendto(struct syscall_frame *regs) {
    sendto_args_t *args = (sendto_args_t *)regs->ebx;
    if (!validate_user_ptr(args, sizeof(sendto_args_t))) return -1;
    if (!current_task) return -1;

    int     fd    = args->fd;
    const void *buf = args->buf;
    uint32_t len  = args->len;
    const struct sockaddr_in *dest = args->dest;

    if (fd < 0 || fd >= MAX_FD) return -1;
    if (!validate_user_ptr(buf, len)) return -1;
    if (!validate_user_ptr(dest, sizeof(struct sockaddr_in))) return -1;

    vfs_node_t *node = current_task->fd_table[fd];
    if (!node || node->type != VFS_SOCKET) return -1;

    ksock_t *ks = ksock_from_node(node);
    if (!ks) return -1;

    if (ks->kind == KS_TCP)
        return tcp_send(ks->proto_idx, (uint8_t *)buf, (uint16_t)len);

    if (ks->kind == KS_UDP) {
        uint32_t dst_ip   = dest->sin_addr;
        uint16_t dst_port = ntohs(dest->sin_port);
        return udp_sock_send(ks->proto_idx, dst_ip, dst_port,
                             (const uint8_t *)buf, (uint16_t)len);
    }
    return -1;
}

static int sys_recvfrom(struct syscall_frame *regs) {
    recvfrom_args_t *args = (recvfrom_args_t *)regs->ebx;
    if (!validate_user_ptr(args, sizeof(recvfrom_args_t))) return -1;
    if (!current_task) return -1;

    int    fd  = args->fd;
    void  *buf = args->buf;
    uint32_t len = args->len;
    struct sockaddr_in *src  = args->src;
    uint32_t           *addrlen = args->addrlen;

    if (fd < 0 || fd >= MAX_FD) return -1;
    if (!validate_user_ptr(buf, len)) return -1;
    if (src && !validate_user_ptr(src, sizeof(struct sockaddr_in))) return -1;
    if (addrlen && !validate_user_ptr(addrlen, sizeof(uint32_t))) return -1;

    vfs_node_t *node = current_task->fd_table[fd];
    if (!node || node->type != VFS_SOCKET) return -1;

    ksock_t *ks = ksock_from_node(node);
    if (!ks) return -1;

    int ret = -1;
    if (ks->kind == KS_TCP) {
        ret = tcp_recv(ks->proto_idx, (uint8_t *)buf, (uint16_t)len);
        if (ret > 0 && src) {
                tcp_socket_t *s = &tcp_sockets[ks->proto_idx];
            src->sin_family = AF_INET;
            src->sin_port   = htons(s->remote_port);
            src->sin_addr   = s->remote_ip;
            if (addrlen) *addrlen = sizeof(struct sockaddr_in);
        }
    } else if (ks->kind == KS_UDP) {
        uint32_t src_ip   = 0;
        uint16_t src_port = 0;
        ret = udp_sock_recv(ks->proto_idx, (uint8_t *)buf, (uint16_t)len,
                            &src_ip, &src_port);
        if (ret > 0 && src) {
            src->sin_family = AF_INET;
            src->sin_port   = htons(src_port);
            src->sin_addr   = htonl(src_ip);
            if (addrlen) *addrlen = sizeof(struct sockaddr_in);
        }
    }
    return ret;
}


typedef struct {
    int         fd;
    int         level;
    int         optname;
    const void *optval;
    uint32_t    optlen;
} setsockopt_args_t;

typedef struct {
    int       fd;
    int       level;
    int       optname;
    void     *optval;
    uint32_t *optlen;
} getsockopt_args_t;

static int sys_shutdown(struct syscall_frame *regs) {
    int fd  = (int)regs->ebx;
    int how = (int)regs->ecx;

    if (!current_task) return -1;
    if (fd < 0 || fd >= MAX_FD) return -1;

    vfs_node_t *node = current_task->fd_table[fd];
    if (!node || node->type != VFS_SOCKET) return -1;

    return ksock_shutdown(node, how);
}

static int sys_setsockopt(struct syscall_frame *regs) {
    setsockopt_args_t *args = (setsockopt_args_t *)regs->ebx;
    if (!validate_user_ptr(args, sizeof(setsockopt_args_t))) return -1;
    if (!current_task) return -1;

    int         fd      = args->fd;
    int         level   = args->level;
    int         optname = args->optname;
    const void *optval  = args->optval;
    uint32_t    optlen  = args->optlen;

    if (fd < 0 || fd >= MAX_FD) return -1;
    if (!optval || optlen < sizeof(int)) return -1;
    if (!validate_user_ptr(optval, optlen)) return -1;

    vfs_node_t *node = current_task->fd_table[fd];
    if (!node || node->type != VFS_SOCKET) return -1;

    ksock_t *ks = ksock_from_node(node);
    if (!ks) return -1;

    int ival = *(const int *)optval;

    if (level == SOL_SOCKET) {
        switch (optname) {
        case SO_REUSEADDR:
            ks->so_reuseaddr = (uint8_t)!!ival;
            return 0;
        case SO_KEEPALIVE:
            ks->so_keepalive = (uint8_t)!!ival;
            if (ks->kind == KS_TCP && ks->proto_idx >= 0 &&
                ks->proto_idx < TCP_MAX_SOCKETS)
                tcp_sockets[ks->proto_idx].keepalive = ks->so_keepalive;
            return 0;
        case SO_ERROR:
            return -1;  
        }
    } else if (level == IPPROTO_TCP) {
        switch (optname) {
        case TCP_NODELAY:
            ks->tcp_nodelay = (uint8_t)!!ival;
            if (ks->kind == KS_TCP && ks->proto_idx >= 0 &&
                ks->proto_idx < TCP_MAX_SOCKETS)
                tcp_sockets[ks->proto_idx].nodelay = ks->tcp_nodelay;
            return 0;
        }
    }
    return -1;
}

static int sys_getsockopt(struct syscall_frame *regs) {
    getsockopt_args_t *args = (getsockopt_args_t *)regs->ebx;
    if (!validate_user_ptr(args, sizeof(getsockopt_args_t))) return -1;
    if (!current_task) return -1;

    int       fd      = args->fd;
    int       level   = args->level;
    int       optname = args->optname;
    void     *optval  = args->optval;
    uint32_t *optlen  = args->optlen;

    if (fd < 0 || fd >= MAX_FD) return -1;
    if (!optval || !optlen) return -1;
    if (!validate_user_ptr(optlen, sizeof(uint32_t))) return -1;
    if (*optlen < sizeof(int)) return -1;
    if (!validate_user_ptr(optval, *optlen)) return -1;

    vfs_node_t *node = current_task->fd_table[fd];
    if (!node || node->type != VFS_SOCKET) return -1;

    ksock_t *ks = ksock_from_node(node);
    if (!ks) return -1;

    int ival = 0;
    if (level == SOL_SOCKET) {
        switch (optname) {
        case SO_REUSEADDR: ival = ks->so_reuseaddr; break;
        case SO_KEEPALIVE: ival = ks->so_keepalive; break;
        case SO_ERROR:
            ival = ks->so_error;
            ks->so_error = 0;  
            break;
        default: return -1;
        }
    } else if (level == IPPROTO_TCP) {
        switch (optname) {
        case TCP_NODELAY: ival = ks->tcp_nodelay; break;
        default: return -1;
        }
    } else {
        return -1;
    }

    *(int *)optval = ival;
    *optlen = sizeof(int);
    return 0;
}

static void deliver_pending_signal(struct task_struct* t,
                                   struct syscall_frame* regs) {
    uint32_t deliverable = t->pending_signals & ~t->signal_mask;
    if (!deliverable) return;

    for (int bit = 0; bit < NSIG; bit++) {
        uint32_t mask = (1u << bit);
        if (!(deliverable & mask)) continue;
        uint32_t handler = t->signal_handlers[bit];
        if (handler <= SIG_IGN) continue;  
        if (handler < USER_SPACE_START || handler >= KERNEL_BASE) continue;

        uint32_t new_esp = regs->useresp - sizeof(signal_frame_t);

        if (new_esp < USER_SPACE_START || new_esp >= KERNEL_BASE) continue;
        uint32_t page_off = new_esp & 0xFFFu;
        if (page_off + sizeof(signal_frame_t) > PAGE_SIZE) continue;

        uint32_t* pd  = t->page_directory;
        if (!pd) continue;
        uint32_t  pdi = PD_INDEX(new_esp);
        uint32_t  pti = PT_INDEX(new_esp);
        if (!(pd[pdi] & PAGE_PRESENT)) continue;
        uint32_t* pt = (uint32_t*)(pd[pdi] & ~0xFFFu);
        if (!(pt[pti] & PAGE_PRESENT)) continue;

        uint32_t phys_page = pt[pti] & ~0xFFFu;
        signal_frame_t* frame = (signal_frame_t*)(phys_page + page_off);

        frame->ret_addr = t->sigreturn_trampoline;
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

        t->pending_signals &= ~mask;
        return; 
    }
}

static int sys_symlink(struct syscall_frame *regs) {
    char *target   = (char *)regs->ebx;
    char *linkpath = (char *)regs->ecx;
    if (!validate_user_str(target))   return -1;
    if (!validate_user_str(linkpath)) return -1;
    if (!current_task) return -1;

    char basename[128];
    vfs_node_t *parent = _resolve_parent_follow(linkpath, basename, 128);
    if (!parent || !basename[0]) return -1;

    return vfs_symlink(parent, basename, target);
}

static int sys_readlink(struct syscall_frame *regs) {
    char     *path  = (char *)regs->ebx;
    char     *buf   = (char *)regs->ecx;
    uint32_t  bufsz = regs->edx;

    if (!validate_user_str(path))          return -1;
    if (!validate_user_ptr(buf, bufsz))    return -1;
    if (bufsz == 0)                        return -1;
    if (!current_task)                     return -1;

    char basename[128];
    vfs_node_t *parent = _resolve_parent_follow(path, basename, 128);
    if (!parent || !basename[0]) return -1;

    vfs_node_t *node = finddir_vfs(parent, basename);
    if (!node) return -1;
    if (node->type != VFS_SYMLINK) return -1;

    return vfs_readlink_node(node, buf, bufsz);
}

static int sys_link(struct syscall_frame *regs) {
    char *oldpath = (char *)regs->ebx;
    char *newpath = (char *)regs->ecx;
    if (!validate_user_str(oldpath)) return -1;
    if (!validate_user_str(newpath)) return -1;
    if (!current_task) return -1;

    vfs_node_t *target_node = _resolve_path(oldpath);
    if (!target_node) return -1;

    char basename[128];
    vfs_node_t *new_parent = _resolve_parent_follow(newpath, basename, 128);
    if (!new_parent || !basename[0]) return -1;

    return vfs_link(new_parent, basename, target_node);
}

static int sys_unlink(char *path) {
    if (!validate_user_str(path)) return -1;
    if (!current_task) return -1;

    char basename[128];
    vfs_node_t *parent = _resolve_parent_follow(path, basename, 128);
    if (!parent || !basename[0]) return -1;
    if (vfs_check_perm(parent, VFS_PERM_WRITE | VFS_PERM_EXEC) < 0) return -1;

    return vfs_unlink(parent, basename);
}

#define SEL_SETSIZE  256
typedef struct { uint32_t w[SEL_SETSIZE / 32]; } sel_fdset_t;

#define SEL_ISSET(fd, s)  (((s)->w[(fd)/32] >> ((fd) % 32)) & 1u)
#define SEL_SET(fd, s)    ((s)->w[(fd)/32] |=  (1u << ((fd) % 32)))
#define SEL_ZERO(s) \
    do { for (int _i = 0; _i < SEL_SETSIZE/32; _i++) (s)->w[_i] = 0; } while (0)

typedef struct {
    int             nfds;
    sel_fdset_t    *readfds;
    sel_fdset_t    *writefds;
    sel_fdset_t    *exceptfds;
    struct timeval *timeout;
} sel_args_t;

#define POLLIN   0x0001
#define POLLOUT  0x0004
#define POLLERR  0x0008
#define POLLHUP  0x0010
#define POLLNVAL 0x0020

struct pollfd {
    int   fd;
    short events;
    short revents;
};


static int fd_read_ready(vfs_node_t *node) {
    if (!node) return 0;
    switch (node->type) {
    case VFS_FILE:
    case VFS_DIRECTORY:
    case VFS_CHARDEVICE:
    case VFS_BLOCKDEVICE:
        return 1;
    case VFS_PIPE: {
        pipe_t *p = (pipe_t *)node->priv;
        return p && (p->len > 0 || !p->write_open);
    }
    case VFS_SOCKET: {
        ksock_t *ks = ksock_from_node(node);
        if (!ks) return 0;
        if (ks->kind == KS_TCP) {
            tcp_socket_t *s = &tcp_sockets[ks->proto_idx];
            return (s->rx_head != s->rx_tail) || s->accept_ready ||
                   (s->state == TCP_CLOSE_WAIT) || (s->state == TCP_CLOSED);
        }
        if (ks->kind == KS_UDP)
            return udp_socks[ks->proto_idx].rx_ready;
        return 0;
    }
    default: return 0;
    }
}

static int fd_write_ready(vfs_node_t *node) {
    if (!node) return 0;
    switch (node->type) {
    case VFS_FILE:
    case VFS_DIRECTORY:
    case VFS_CHARDEVICE:
    case VFS_BLOCKDEVICE:
        return 1;
    case VFS_PIPE: {
        pipe_t *p = (pipe_t *)node->priv;
        return p && (p->len < PIPE_BUF_SIZE) && p->read_open;
    }
    case VFS_SOCKET: {
        ksock_t *ks = ksock_from_node(node);
        if (!ks) return 0;
        if (ks->kind == KS_TCP) {
            tcp_socket_t *s = &tcp_sockets[ks->proto_idx];
            return (s->state == TCP_ESTABLISHED) || (s->state == TCP_CLOSE_WAIT);
        }
        if (ks->kind == KS_UDP) return 1;  
        return 0;
    }
    default: return 0;
    }
}

static int sys_select(struct syscall_frame *regs) {
    sel_args_t *ua = (sel_args_t *)regs->ebx;
    if (!validate_user_ptr(ua, sizeof(sel_args_t))) return -1;
    if (!current_task) return -1;

    int             nfds  = ua->nfds;
    sel_fdset_t    *urfds = ua->readfds;
    sel_fdset_t    *uwfds = ua->writefds;
    sel_fdset_t    *uefds = ua->exceptfds;
    struct timeval *utv   = ua->timeout;

    if (nfds < 0 || nfds > MAX_FD)                                     return -1;
    if (urfds && !validate_user_ptr(urfds, sizeof(sel_fdset_t)))        return -1;
    if (uwfds && !validate_user_ptr(uwfds, sizeof(sel_fdset_t)))        return -1;
    if (uefds && !validate_user_ptr(uefds, sizeof(sel_fdset_t)))        return -1;
    if (utv   && !validate_user_ptr(utv,   sizeof(struct timeval)))     return -1;

    int      infinite    = (utv == 0);
    int      nonblocking = 0;
    uint32_t deadline    = 0;
    if (!infinite) {
        uint32_t ticks = (uint32_t)(utv->tv_sec * TIMER_HZ) +
                         (uint32_t)((utv->tv_usec + (1000000 / TIMER_HZ) - 1) /
                                    (1000000 / TIMER_HZ));
        nonblocking = (ticks == 0);
        deadline    = timer_ticks_get() + ticks;
    }

    sel_fdset_t orig_r, orig_w, orig_e;
    SEL_ZERO(&orig_r); SEL_ZERO(&orig_w); SEL_ZERO(&orig_e);
    if (urfds) orig_r = *urfds;
    if (uwfds) orig_w = *uwfds;
    if (uefds) orig_e = *uefds;

    struct task_struct *t = current_task;

    for (;;) {
        sel_fdset_t res_r, res_w, res_e;
        SEL_ZERO(&res_r); SEL_ZERO(&res_w); SEL_ZERO(&res_e);
        int ready = 0;

        for (int fd = 0; fd < nfds; fd++) {
            vfs_node_t *node = t->fd_table[fd];

            if (urfds && SEL_ISSET(fd, &orig_r)) {
                if (node && fd_read_ready(node)) { SEL_SET(fd, &res_r); ready++; }
            }
            if (uwfds && SEL_ISSET(fd, &orig_w)) {
                if (node && fd_write_ready(node)) { SEL_SET(fd, &res_w); ready++; }
            }
            if (uefds && SEL_ISSET(fd, &orig_e)) {
                if (node && node->type == VFS_PIPE) {
                    pipe_t *p = (pipe_t *)node->priv;
                    if (p && !p->write_open) { SEL_SET(fd, &res_e); ready++; }
                }
            }
        }

        if (ready > 0 || nonblocking ||
                (!infinite && (int32_t)(timer_ticks_get() - deadline) >= 0)) {
            if (urfds) *urfds = res_r;
            if (uwfds) *uwfds = res_w;
            if (uefds) *uefds = res_e;
            return ready;
        }

        schedule();  
    }
}

static int sys_poll(struct syscall_frame *regs) {
    struct pollfd *fds        = (struct pollfd *)regs->ebx;
    int            nfds       = (int)regs->ecx;
    int            timeout_ms = (int)regs->edx;

    if (!current_task) return -1;
    if (nfds <= 0)     return 0;
    if (!validate_user_ptr(fds, (uint32_t)nfds * sizeof(struct pollfd))) return -1;

    int      infinite    = (timeout_ms < 0);
    int      nonblocking = (timeout_ms == 0);
    uint32_t deadline    = 0;
    if (!infinite && !nonblocking) {
        uint32_t ticks = (uint32_t)((timeout_ms + (1000 / TIMER_HZ) - 1) /
                                    (1000 / TIMER_HZ));
        deadline = timer_ticks_get() + ticks;
    }

    struct task_struct *t = current_task;

    for (;;) {
        int ready = 0;

        for (int i = 0; i < nfds; i++) {
            fds[i].revents = 0;
            int fd = fds[i].fd;
            if (fd < 0 || fd >= MAX_FD) { fds[i].revents = POLLNVAL; continue; }
            vfs_node_t *node = t->fd_table[fd];
            if (!node)                  { fds[i].revents = POLLNVAL; continue; }

            short ev = 0;
            if ((fds[i].events & POLLIN)  && fd_read_ready(node))  ev |= POLLIN;
            if ((fds[i].events & POLLOUT) && fd_write_ready(node)) ev |= POLLOUT;

            /* POLLHUP/POLLERR for broken pipes (reported regardless of events) */
            if (node->type == VFS_PIPE) {
                pipe_t *pp = (pipe_t *)node->priv;
                if (pp) {
                    int is_wr = (int)node->inode;
                    if (!is_wr && !pp->write_open && pp->len == 0)
                        ev |= POLLHUP;     /* read end: writer gone, no data */
                    if (is_wr && !pp->read_open)
                        ev |= POLLERR;     /* write end: reader gone */
                }
            }

            fds[i].revents = ev;
            if (ev) ready++;
        }

        if (ready > 0 || nonblocking ||
                (!infinite && (int32_t)(timer_ticks_get() - deadline) >= 0))
            return ready;

        schedule();
    }
}


static int sys_shmget(struct syscall_frame* regs) {
    int      key   = (int)regs->ebx;
    uint32_t size  = regs->ecx;
    int      flags = (int)regs->edx;
    return shm_get(key, size, flags);
}

static int sys_shmat(struct syscall_frame* regs) {
    int      shmid   = (int)regs->ebx;
    uint32_t shmaddr = regs->ecx;
    int      flags   = (int)regs->edx;
    return (int)shm_at(shmid, shmaddr, flags);
}

static int sys_shmdt(struct syscall_frame* regs) {
    uint32_t shmaddr = regs->ebx;
    return shm_dt(shmaddr);
}

static int sys_shmctl(struct syscall_frame* regs) {
    int   shmid = (int)regs->ebx;
    int   cmd   = (int)regs->ecx;
    void* buf   = (void*)regs->edx;
    if (buf && !validate_user_ptr(buf, 64)) return -1;
    return shm_ctl(shmid, cmd, buf);
}

/* All syscalls go through the cdecl ABI: callers push up to 3 args
 * (ebx/ecx/edx) and the callee pops nothing.  Some real prototypes take
 * a struct syscall_frame*, others take a few void*; both are reachable
 * via casts.  Declare the table-entry type as the widest 3-arg shape so
 * that the dispatch site below is a well-typed call. */
typedef int (*syscall_fn)(void*, void*, void*);
static syscall_fn syscall_table[] = {
    [0]  = (syscall_fn)sys_print,
    [1]  = (syscall_fn)sys_get_pid,
    [2]  = (syscall_fn)sys_open,
    [3]  = (syscall_fn)sys_read,
    [4]  = (syscall_fn)sys_write,
    [5]  = (syscall_fn)sys_create,
    [6]  = (syscall_fn)sys_delete,
    [7]  = (syscall_fn)sys_exit,
    [8]  = (syscall_fn)sys_close,
    [9]  = (syscall_fn)sys_fork,
    [10] = (syscall_fn)sys_exec,
    [11] = (syscall_fn)sys_kill,
    [12] = (syscall_fn)sys_signal,
    [13] = (syscall_fn)sys_mmap,
    [14] = (syscall_fn)sys_munmap,
    [15] = (syscall_fn)sys_mprotect,
    [16] = (syscall_fn)sys_sigreturn,   
    [17] = (syscall_fn)sys_sigaction,   
    [18] = (syscall_fn)sys_pipe,     
    [19] = (syscall_fn)sys_dup2,
    [20] = (syscall_fn)sys_lseek,
    [21] = (syscall_fn)sys_waitpid,
    [22] = (syscall_fn)sys_sleep,
    [23] = (syscall_fn)sys_brk,
    [24] = (syscall_fn)sys_getcwd,
    [25] = (syscall_fn)sys_chdir,
    [26] = (syscall_fn)sys_stat,
    [27] = (syscall_fn)sys_fstat,
    [28] = (syscall_fn)sys_getppid,
    [29] = (syscall_fn)sys_getdents,
    [30] = (syscall_fn)sys_rename,
    [31] = (syscall_fn)sys_ioctl,
    [32] = (syscall_fn)sys_mkdir,
    [33] = (syscall_fn)sys_rmdir,
    [34] = (syscall_fn)sys_fcntl,
    [35] = (syscall_fn)sys_gettimeofday,
    [36] = (syscall_fn)sys_clock_gettime,
    [37] = (syscall_fn)sys_nanosleep,
    [38] = (syscall_fn)sys_sigprocmask,
    [39] = (syscall_fn)sys_sigpending,
    [40] = (syscall_fn)sys_sigsuspend,
    [41] = (syscall_fn)sys_alarm,
    [42] = (syscall_fn)sys_setitimer,
    [43] = (syscall_fn)sys_socket,
    [44] = (syscall_fn)sys_bind,
    [45] = (syscall_fn)sys_connect,
    [46] = (syscall_fn)sys_listen,
    [47] = (syscall_fn)sys_accept,
    [48] = (syscall_fn)sys_send,
    [49] = (syscall_fn)sys_recv,
    [50] = (syscall_fn)sys_sendto,
    [51] = (syscall_fn)sys_recvfrom,
    [52] = (syscall_fn)sys_shutdown,
    [53] = (syscall_fn)sys_setsockopt,
    [54] = (syscall_fn)sys_symlink,
    [55] = (syscall_fn)sys_readlink,
    [56] = (syscall_fn)sys_link,
    [57] = (syscall_fn)sys_unlink,
    [58] = (syscall_fn)sys_select,
    [59] = (syscall_fn)sys_poll,
    [60] = (syscall_fn)sys_getsockopt,
    [61] = (syscall_fn)sys_shmget,
    [62] = (syscall_fn)sys_shmat,
    [63] = (syscall_fn)sys_shmdt,
    [64] = (syscall_fn)sys_shmctl,
    [65] = (syscall_fn)sys_getuid,
    [66] = (syscall_fn)sys_getgid,
    [67] = (syscall_fn)sys_setuid,
    [68] = (syscall_fn)sys_setgid,
    [69] = (syscall_fn)sys_geteuid,
    [70] = (syscall_fn)sys_getegid,
    [71] = (syscall_fn)sys_chmod,
    [72] = (syscall_fn)sys_chown,
};
#define SYSCALL_COUNT (sizeof(syscall_table)/sizeof(syscall_table[0]))

void syscall_handler(struct syscall_frame* regs) {
    uint32_t num = regs->eax;
    if (num >= SYSCALL_COUNT || !syscall_table[num]) {
        regs->eax = (uint32_t)-1;
        return;
    }

    int ret;
    if (num == 7 || num == 9 || num == 10 || num == 13 || num == 14 ||
        num == 15 || num == 16 || num == 17 || num == 18 || num == 19 ||
        num == 20 || num == 21 || num == 22 || num == 23 ||
        num == 24 || num == 25 || num == 26 || num == 27 ||
        num == 29 || num == 31 ||
        num == 35 || num == 36 || num == 37 ||
        num == 38 || num == 39 || num == 40 || num == 41 || num == 42 ||
        num == 43 || num == 44 || num == 45 || num == 46 || num == 47 ||
        num == 48 || num == 49 || num == 50 || num == 51 ||
        num == 52 || num == 53 || num == 60 ||
        num == 54 || num == 55 || num == 56 ||
        num == 58 || num == 59 ||
        num == 61 || num == 62 || num == 63 || num == 64) {
        ret = ((int(*)(struct syscall_frame*))syscall_table[num])(regs);
    } else {
        ret = syscall_table[num](
            (void*)regs->ebx,
            (void*)regs->ecx,
            (void*)regs->edx
        );
    }

    if (num == 7) return;
    regs->eax = (uint32_t)ret;

    if (current_task && !current_task->is_kernel)
        deliver_pending_signal(current_task, regs);
}