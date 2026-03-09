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
    return read_vfs(node, 0, size, buf);
}

static int sys_write(int fd, char* buf, unsigned int size) {
    if (!size)                         return -1;
    if (!validate_user_ptr(buf, size)) return -1;
    if (!current_task)                 return -1;
    if (fd < 0 || fd >= MAX_FD)        return -1;
    struct vfs_node* node = current_task->fd_table[fd];
    if (!node) return -1;
    return write_vfs(node, 0, size, buf);
}

static int sys_close(int fd) {
    if (!current_task)          return -1;
    if (fd < 3 || fd >= MAX_FD) return -1;
    struct vfs_node* node = current_task->fd_table[fd];
    if (!node) return -1;
    current_task->fd_table[fd] = 0;
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

static int sys_exit() {
    if (current_task) current_task->state = TASK_ZOMBIE;
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

static int sys_exec(struct syscall_frame* regs) {
    char* path = (char*)regs->ebx;
    if (!validate_user_str(path)) return -1;
    struct context_frame cf;
    cf.eip     = regs->eip;
    cf.cs      = regs->cs;
    cf.eflags  = regs->eflags;
    cf.useresp = regs->useresp;
    cf.ss      = regs->ss;
    return task_exec(path, &cf);
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
};
#define SYSCALL_COUNT (sizeof(syscall_table)/sizeof(syscall_table[0]))

void syscall_handler(struct syscall_frame* regs) {
    uint32_t num = regs->eax;
    if (num >= SYSCALL_COUNT || !syscall_table[num]) {
        regs->eax = (uint32_t)-1;
        return;
    }

    int ret;
    if (num == 9 || num == 10 || num == 13 || num == 14 || num == 15 || num == 16 || num == 17 || num == 18 || num == 19) {
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