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

static int sys_print(char* msg) {
    if (!validate_user_str(msg)) return -1;
    kprint(msg);
    return 0;
}

static int sys_get_pid() {
    return (int)current_task->pid;
}

static int sys_open(char* name) {
    if (!validate_user_str(name)) return -1;
    if (!current_task) return -1;
    struct vfs_node* node = finddir_vfs(vfs_root, name);
    if (!node) return -1;
    for (int i = 3; i < MAX_FD; i++) {
        if (!current_task->fd_table[i]) {
            current_task->fd_table[i] = node;
            current_task->fd_offset[i] = 0;
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
    return create_vfs(vfs_root, name);
}

static int sys_delete(char* name) {
    if (!validate_user_str(name)) return -1;
    return delete_vfs(vfs_root, name);
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

    current_task->wait_for_pid = (target_pid > 0) ? (uint32_t)target_pid : 0;
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
        num == 20 || num == 21 || num == 22 || num == 23) {
        ret = ((int(*)(struct syscall_frame*))syscall_table[num])(regs);
    } else {
        ret = syscall_table[num](
            (void*)regs->ebx,
            (void*)regs->ecx,
            (void*)regs->edx
        );
    }

    regs->eax = (uint32_t)ret;
}