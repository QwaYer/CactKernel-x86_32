#include "kernel.h"
#include "task.h"
#include "vfs.h"
#include "memory.h"

struct syscall_frame {
    uint32_t es, ds;
    uint32_t edi, esi, ebp, esp_dummy, ebx, edx, ecx, eax;
    uint32_t eip, cs, eflags, useresp, ss;
} __attribute__((packed));

static int sys_print(char* msg) {
    if (!msg) return -1;
    kprint(msg);
    return 0;
}

static int sys_get_pid() {
    return (int)current_task->pid;
}

static int sys_open(char* name) {
    if (!name) return -1;
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
    if (!buf || !size) return -1;
    if (!current_task) return -1;
    if (fd < 0 || fd >= MAX_FD) return -1;
    struct vfs_node* node = current_task->fd_table[fd];
    if (!node) return -1;
    return read_vfs(node, 0, size, buf);
}

static int sys_write(int fd, char* buf, unsigned int size) {
    if (!buf || !size) return -1;
    if (!current_task) return -1;
    if (fd < 0 || fd >= MAX_FD) return -1;
    struct vfs_node* node = current_task->fd_table[fd];
    if (!node) return -1;
    return write_vfs(node, 0, size, buf);
}

static int sys_close(int fd) {
    if (!current_task) return -1;
    if (fd < 3 || fd >= MAX_FD) return -1;
    current_task->fd_table[fd] = 0;
    return 0;
}

static int sys_create(char* name) {
    if (!name) return -1;
    return create_vfs(vfs_root, name);
}

static int sys_delete(char* name) {
    if (!name) return -1;
    return delete_vfs(vfs_root, name);
}

static int sys_exit() {
    if (current_task) current_task->state = TASK_ZOMBIE;
    return 0;
}

static int sys_fork(struct syscall_frame* regs) {
    struct context_frame cf;
    cf.es = regs->es; cf.ds = regs->ds;
    cf.edi = regs->edi; cf.esi = regs->esi;
    cf.ebp = regs->ebp; cf.esp_dummy = regs->esp_dummy;
    cf.ebx = regs->ebx; cf.edx = regs->edx;
    cf.ecx = regs->ecx; cf.eax = regs->eax;
    cf.eip = regs->eip; cf.cs = regs->cs;
    cf.eflags = regs->eflags;
    cf.useresp = regs->useresp; cf.ss = regs->ss;

    struct task_struct* child = task_fork(&cf);
    if (!child) return -1;
    return (int)child->pid;
}

static int sys_exec(struct syscall_frame* regs) {
    char* path = (char*)regs->ebx;
    if (!path) return -1;

    struct context_frame cf;
    cf.eip = regs->eip; cf.cs = regs->cs;
    cf.eflags = regs->eflags;
    cf.useresp = regs->useresp; cf.ss = regs->ss;

    return task_exec(path, &cf);
}

static int sys_kill(uint32_t pid) {
    task_kill(pid);
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
};
#define SYSCALL_COUNT (sizeof(syscall_table)/sizeof(syscall_table[0]))

void syscall_handler(struct syscall_frame* regs) {
    uint32_t num = regs->eax;
    if (num >= SYSCALL_COUNT || !syscall_table[num]) {
        regs->eax = (uint32_t)-1;
        return;
    }

    int ret;
    if (num == 9 || num == 10) {
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