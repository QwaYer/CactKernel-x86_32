#ifndef TASK_H
#define TASK_H

typedef enum {
    TASK_READY,
    TASK_RUNNING,
    TASK_SLEEPING,
    TASK_ZOMBIE
} task_state;

struct context_frame {
    unsigned int edi, esi, ebp, esp_dummy, ebx, edx, ecx, eax; 
    unsigned int eip, cs, eflags; 
};

struct task_struct {
    unsigned int esp;           
    unsigned int pid;     
    task_state state;       
    void* stack_base;     
    struct task_struct* next;    
};

void task_init();
struct task_struct* create_task(void (*entry_point)());
void schedule();
int init_scheduler();

extern struct task_struct* volatile current_task;
extern struct task_struct* volatile task_list_head;
extern unsigned int next_pid;

#endif