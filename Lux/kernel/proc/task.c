#include "task.h"
#include "memory.h"
#include "kernel.h"

struct task_struct* volatile current_task = 0;
struct task_struct* volatile task_list_head = 0;
unsigned int next_pid = 1;

void task_init() {
    current_task = 0;
    task_list_head = 0;
    next_pid = 1;
}

int init_scheduler() {
    return 0;
}

struct task_struct* create_task(void (*entry_point)()) {
    struct task_struct* new_task = (struct task_struct*)kmalloc(sizeof(struct task_struct));
    if (!new_task) return 0;

    void* stack = kalloc(); 
    if (!stack) return 0;

    new_task->stack_base = stack;
    new_task->pid = next_pid++;
    new_task->state = TASK_READY;
    new_task->next = 0;

    unsigned int* stack_ptr = (unsigned int*)((unsigned char*)stack + 4096);

    *(--stack_ptr) = 0x202;         
    *(--stack_ptr) = 0x08;       
    *(--stack_ptr) = (unsigned int)entry_point; 

    for (int i = 0; i < 8; i++) {
        *(--stack_ptr) = 0;
    }

    new_task->esp = (unsigned int)stack_ptr;

    if (!task_list_head) {
        task_list_head = new_task;
    } else {
        struct task_struct* tmp = task_list_head;
        while (tmp->next) {
            tmp = tmp->next;
        }
        tmp->next = new_task;
    }

    return new_task;
}

void schedule() {
    if (current_task == 0) {
        if (task_list_head != 0) {
            current_task = task_list_head;
        }
        return;
    }

    if (current_task->next) {
        current_task = current_task->next;
    } else {
        current_task = task_list_head;
    }
}