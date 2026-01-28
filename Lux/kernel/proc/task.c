#include "task.h"
#include "memory.h"
#include "libc.h"
#include "gdt.h"

extern void terminal_task();
extern void switch_to(unsigned int* old_esp, unsigned int new_esp);
extern struct tss_entry_struct tss_entry;

struct task_struct* volatile current_task = 0;
struct task_struct* volatile task_list_head = 0;
unsigned int next_pid = 1;

void task_init() {
    current_task = 0;
    task_list_head = 0;
    next_pid = 1;
}

struct task_struct* create_task(void (*entry_point)()) {
    struct task_struct* new_task = (struct task_struct*)kmalloc(sizeof(struct task_struct));
    if (!new_task) return 0;

    unsigned int* stack = (unsigned int*)kalloc();
    if (!stack) return 0;

    unsigned int* esp = (unsigned int*)((unsigned int)stack + 4096);

    *(--esp) = (unsigned int)entry_point; 
    *(--esp) = 0; 
    *(--esp) = 0; 
    *(--esp) = 0; 
    *(--esp) = 0; 

    new_task->esp = (unsigned int)esp;
    new_task->stack_base = (void*)stack;
    new_task->pid = next_pid++;
    new_task->state = 1; 

    if (!task_list_head) {
        task_list_head = new_task;
        new_task->next = new_task;
    } else {
        struct task_struct* last = task_list_head;
        while (last->next != task_list_head) {
            last = last->next;
        }
        last->next = new_task;
        new_task->next = task_list_head;
    }

    return new_task;
}

int init_scheduler() {
    current_task = (struct task_struct*)kmalloc(sizeof(struct task_struct));
    current_task->pid = 0;
    current_task->stack_base = 0; 
    current_task->state = 2;     
    current_task->next = current_task;
    task_list_head = current_task;

    create_task(terminal_task);
    
    return 0;
}

void schedule() {
    if (!task_list_head || !current_task) return;

    struct task_struct* prev = current_task;
    current_task = current_task->next;

    tss_entry.esp0 = (unsigned int)current_task->stack_base + 4096;

    switch_to(&(prev->esp), current_task->esp);
}

void list_tasks() {
    if (!task_list_head) return;
    struct task_struct* tmp = task_list_head;
    kprint("\nPID  STATE\n");
    do {
        char buf[16];
        itoa(tmp->pid, buf);
        kprint(buf);
        kprint(tmp->state == 2 ? "  RUNNING\n" : "  READY\n");
        tmp = tmp->next;
    } while (tmp != task_list_head);
}