#include "kernel.h"
#include "task.h"

void sys_print(char* msg) {
    kprint(msg);
}

int sys_get_pid() {
    return current_task->pid;
}

static void* syscall_table[] = {
    [0] = sys_print,
    [1] = sys_get_pid
};

#define MAX_SYSCALL 1

void syscall_handler(struct context_frame* regs) {
    unsigned int syscall_num = regs->eax;

    if (syscall_num > MAX_SYSCALL) return;

    void* location = syscall_table[syscall_num];
    if (!location) return; 

    if (syscall_num == 0) { 
        void (*handler)(char*) = (void (*)(char*))location;
        handler((char*)regs->ebx); 
    } else {
        void (*handler)() = (void (*)())location;
        handler();
    }
}