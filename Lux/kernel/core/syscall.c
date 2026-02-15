#include "kernel.h"
#include "task.h"
#include "vfs.h"

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
static int sys_get_pid()  { return (int)current_task->pid; }
static int sys_open(char* name) {
    if (!name) return 0;
    return (int)finddir_vfs(vfs_root, name);
}
static int sys_read(struct vfs_node* n, char* b, unsigned int s) {
    if (!n||!b||!s) return -1;
    return read_vfs(n, 0, s, b);
}
static int sys_write(struct vfs_node* n, char* b, unsigned int s) {
    if (!n||!b||!s) return -1;
    return write_vfs(n, 0, s, b);
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
    return 0; /* syscall_isr увидит ZOMBIE и переключится */
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
#define SYSCALL_COUNT (sizeof(syscall_table)/sizeof(syscall_table[0]))

void syscall_handler(struct syscall_frame* regs) {
    uint32_t num = regs->eax;
    if (num >= SYSCALL_COUNT || !syscall_table[num]) {
        regs->eax = (uint32_t)-1;
        return;
    }
    int ret = syscall_table[num](
        (void*)regs->ebx,
        (void*)regs->ecx,
        (void*)regs->edx
    );
    regs->eax = (uint32_t)ret;
}