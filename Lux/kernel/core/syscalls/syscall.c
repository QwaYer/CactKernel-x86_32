#include "kernel.h"
#include "task.h"
#include "vfs.h"
#include "memory.h"
#include "mmap.h"
#include "pipe.h"

struct syscall_frame {
    uint32_t es, ds;
    uint32_t edi, esi, ebp, esp_dummy, ebx, edx, ecx, eax;
    uint32_t eip, cs, eflags, useresp, ss;
} __attribute__((packed));

#define KERNEL_BASE   0xC0000000U
#define USER_STR_MAX  4096

static int validate_user_ptr(const void* ptr, uint32_t size) {
    uint32_t addr = (uint32_t)ptr;
    if (!ptr)                return 0;
    if (addr >= KERNEL_BASE) return 0;
    if (size == 0)           return 1;
    uint32_t end = addr + size;
    if (end < addr)          return 0;
    if (end > KERNEL_BASE)   return 0;
    return 1;
}

static int validate_user_str(const char* str) {
    uint32_t addr = (uint32_t)str;
    if (!str)                return 0;
    if (addr >= KERNEL_BASE) return 0;
    for (uint32_t i = 0; i < USER_STR_MAX; i++) {
        if (addr + i >= KERNEL_BASE) return 0;
        if (str[i] == '\0')          return 1;
    }
    return 0;
}

// resolve user path (absolute or relative to CWD) -> vfs_node
static vfs_node_t* _resolve_path(const char* path) {
    if (!path || !current_task) return 0;
    if (path[0] == '/') {
        return vfs_walk_path(vfs_root, path);
    }
    char abs[512];
    int p = 0;
    for (int i = 0; current_task->cwd[i] && p < 510; i++)
        abs[p++] = current_task->cwd[i];
    if (p > 0 && abs[p-1] != '/') abs[p++] = '/';
    for (int i = 0; path[i] && p < 511; i++)
        abs[p++] = path[i];
    abs[p] = '\0';
    return vfs_walk_path(vfs_root, abs);
}

// resolve parent directory and extract basename for create/delete
static vfs_node_t* _resolve_parent(const char* path, char* basename_out, int basename_max) {
    if (!path || !current_task) return 0;

    char abs[512];
    if (path[0] == '/') {
        int i = 0;
        while (path[i] && i < 511) { abs[i] = path[i]; i++; }
        abs[i] = '\0';
    } else {
        int p = 0;
        for (int i = 0; current_task->cwd[i] && p < 510; i++)
            abs[p++] = current_task->cwd[i];
        if (p > 0 && abs[p-1] != '/') abs[p++] = '/';
        for (int i = 0; path[i] && p < 511; i++)
            abs[p++] = path[i];
        abs[p] = '\0';
    }

    int last_slash = -1;
    for (int i = 0; abs[i]; i++) {
        if (abs[i] == '/') last_slash = i;
    }

    if (last_slash < 0) {
        int i = 0;
        while (path[i] && i < basename_max - 1) { basename_out[i] = path[i]; i++; }
        basename_out[i] = '\0';
        return vfs_walk_path(vfs_root, current_task->cwd);
    }

    const char* bn = abs + last_slash + 1;
    int i = 0;
    while (bn[i] && i < basename_max - 1) { basename_out[i] = bn[i]; i++; }
    basename_out[i] = '\0';

    if (last_slash == 0) {
        return vfs_root;
    }

    char parent_path[512];
    for (int j = 0; j < last_slash && j < 511; j++)
        parent_path[j] = abs[j];
    parent_path[last_slash] = '\0';

    return vfs_walk_path(vfs_root, parent_path);
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

static int sys_open(char* name) {
    if (!validate_user_str(name)) return -1;
    if (!current_task) return -1;

    vfs_node_t* node = _resolve_path(name);
    if (!node) {
        kprint("[DBG] sys_open: not found: "); kprint(name); kprint("\n");
        return -1;
    }

    for (int i = 3; i < MAX_FD; i++) {
        if (!current_task->fd_table[i]) {
            current_task->fd_table[i] = node;
            current_task->fd_offset[i] = 0;
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
    if (fd < 3 || fd >= MAX_FD) return -1;
    struct vfs_node* node = current_task->fd_table[fd];
    if (!node) return -1;
    current_task->fd_table[fd] = 0;
    current_task->fd_offset[fd] = 0;
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

static int sys_exit(struct syscall_frame* regs) {
    if (!current_task) return -1;
    current_task->exit_code = (int)regs->ebx;
    current_task->state = TASK_ZOMBIE;

    irq_spinlock_acquire(&scheduler_lock);
    struct task_struct* t = task_list_head;
    if (t) {
        do {
            if (t->state == TASK_WAITING &&
                (t->wait_for_pid == current_task->pid || t->wait_for_pid == 0) &&
                t->pid == current_task->parent_pid) {
                sched_queue_remove(&wait_queue, t);
                t->state = TASK_READY;
                sched_queue_push(&ready_queue, t);

                uint32_t* parent_frame = (uint32_t*)t->esp;
                parent_frame[9] = current_task->pid;  // eax = child pid

                int* status_ptr = (int*)parent_frame[8];
                if (status_ptr && (uint32_t)status_ptr < 0xC0000000u) {
                    uint32_t va = (uint32_t)status_ptr;
                    uint32_t pdi = PD_INDEX(va);
                    uint32_t pti = PT_INDEX(va);
                    if (t->page_directory &&
                        (t->page_directory[pdi] & PAGE_PRESENT)) {
                        uint32_t* pt = (uint32_t*)(t->page_directory[pdi] & ~0xFFFu);
                        if (pt[pti] & PAGE_PRESENT) {
                            uint32_t phys = (pt[pti] & ~0xFFFu) + (va & 0xFFFu);
                            *(int*)phys = current_task->exit_code;
                        }
                    }
                }
                break;
            }
            t = t->next;
        } while (t != task_list_head);
    }
    irq_spinlock_release(&scheduler_lock);
    return 0;
}

static int sys_fork(struct syscall_frame* regs) {
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
    if (!child) return -1;
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
            if ((uint32_t)&argv[i] >= KERNEL_BASE) return -1;
            if (!argv[i]) break;
            if (!validate_user_str(argv[i])) return -1;
        }
    }

    if (envp) {
        if (!validate_user_ptr(envp, sizeof(char*))) return -1;
        for (int i = 0; i < EXEC_VALIDATE_MAX; i++) {
            if ((uint32_t)&envp[i] >= KERNEL_BASE) return -1;
            if (!envp[i]) break;
            if (!validate_user_str(envp[i])) return -1;
        }
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

    current_task->fd_table[rfd] = pipefd[0];
    current_task->fd_table[wfd] = pipefd[1];
    current_task->fd_offset[rfd] = 0;
    current_task->fd_offset[wfd] = 0;

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

    struct vfs_node* node = current_task->fd_table[oldfd];
    if (!node) return -1;

    if (current_task->fd_table[newfd]) {
        close_vfs(current_task->fd_table[newfd]);
    }

    current_task->fd_table[newfd] = node;
    current_task->fd_offset[newfd] = current_task->fd_offset[oldfd];
    open_vfs(node);   
    return newfd;
}

static int sys_sigaction(struct syscall_frame* regs) {
    uint32_t signum  = regs->ebx;
    uint32_t handler = regs->ecx;

    if (!current_task) return -1;

    if (handler > SIG_IGN && handler >= 0xC0000000u) return -1;

    return task_sigaction(current_task, signum, handler);
}

static int sys_sigreturn(struct syscall_frame* regs) {
    if (!current_task || current_task->is_kernel) return -1;

    uint32_t user_esp = regs->useresp;
    if (user_esp >= 0xC0000000u) return -1;

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

extern uint32_t timer_ticks_get(void);

static int sys_waitpid(struct syscall_frame* regs) {
    int target_pid = (int)regs->ebx;
    int* status    = (int*)regs->ecx;

    if (!current_task) return -1;
    if (status && !validate_user_ptr(status, sizeof(int))) return -1;

    irq_spinlock_acquire(&scheduler_lock);
    struct task_struct* t = task_list_head;
    if (t) {
        do {
            if (t->state == TASK_ZOMBIE &&
                t->parent_pid == current_task->pid &&
                (target_pid <= 0 || t->pid == (uint32_t)target_pid)) {
                uint32_t child_pid = t->pid;
                int child_exit = t->exit_code;
                irq_spinlock_release(&scheduler_lock);
                if (status) *status = child_exit;
                return (int)child_pid;
            }
            t = t->next;
        } while (t != task_list_head);
    }

    current_task->wait_for_pid = (target_pid > 0) ?
        (uint32_t)target_pid : 0;
    current_task->state = TASK_WAITING;
    irq_spinlock_release(&scheduler_lock);

    return -2;
}

static int sys_sleep(struct syscall_frame* regs) {
    uint32_t ms = regs->ebx;
    if (!current_task) return -1;
    if (ms == 0) return 0;

    uint32_t ticks = (ms + 9) / 10;
    uint32_t now = timer_ticks_get();
    current_task->sleep_until = now + ticks;

    current_task->state = TASK_SLEEPING;
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

    // normalize "." and ".."
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

typedef int (*syscall_fn)();
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
        num == 24 || num == 25 || num == 26 || num == 27) {
        ret = ((int(*)(struct syscall_frame*))syscall_table[num])(regs);
    } else {
        ret = syscall_table[num](
            (void*)regs->ebx,
            (void*)regs->ecx,
            (void*)regs->edx
        );
    }

    // sys_exit (7): task is ZOMBIE, don't touch its frame
    if (num == 7) return;

    regs->eax = (uint32_t)ret;
}