#include "kernel.h"
#include "task.h"
#include "vfs.h"


/* 0: print(str) */
static int sys_print(char* msg) {
    if (!msg) return -1;
    kprint(msg);
    return 0;
}

/* 1: get_pid() */
static int sys_get_pid() {
    return (int)current_task->pid;
}

/* 2: open(name) -> vfs_node* */
static int sys_open(char* name) {
    if (!name) return 0;
    return (int)finddir_vfs(vfs_root, name);
}

/* 3: read(node*, buf, size) */
static int sys_read(struct vfs_node* node, char* buf, unsigned int size) {
    if (!node || !buf || size == 0) return -1;
    return read_vfs(node, 0, size, buf);
}

/* 4: write(node*, buf, size) */
static int sys_write(struct vfs_node* node, char* buf, unsigned int size) {
    if (!node || !buf || size == 0) return -1;
    return write_vfs(node, 0, size, buf);
}

/* 5: create(name) */
static int sys_create(char* name) {
    if (!name) return -1;
    return create_vfs(vfs_root, name);
}

/* 6: delete(name) */
static int sys_delete(char* name) {
    if (!name) return -1;
    return delete_vfs(vfs_root, name);
}

/* 7: exit() */
static int sys_exit() {
    if (current_task) current_task->state = TASK_ZOMBIE;
    while (1) __asm__ __volatile__("hlt");
    return 0;
}

typedef int (*syscall_fn)();

static syscall_fn syscall_table[] = {
    [0] = (syscall_fn)sys_print,
    [1] = (syscall_fn)sys_get_pid,
    [2] = (syscall_fn)sys_open,
    [3] = (syscall_fn)sys_read,
    [4] = (syscall_fn)sys_write,
    [5] = (syscall_fn)sys_create,
    [6] = (syscall_fn)sys_delete,
    [7] = (syscall_fn)sys_exit,
};

#define SYSCALL_COUNT (sizeof(syscall_table) / sizeof(syscall_table[0]))

void syscall_handler(struct context_frame* regs) {
    unsigned int num = regs->eax;

    if (num >= SYSCALL_COUNT) {
        regs->eax = (unsigned int)-1;
        return;
    }

    syscall_fn fn = syscall_table[num];
    if (!fn) {
        regs->eax = (unsigned int)-1;
        return;
    }

    int ret = fn((void*)regs->ebx, (void*)regs->ecx, (void*)regs->edx);
    regs->eax = (unsigned int)ret;
}