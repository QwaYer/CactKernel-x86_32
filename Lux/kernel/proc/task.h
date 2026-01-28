#ifndef TASK_H
#define TASK_H

#define USER_CODE_SEL 0x1B
#define USER_DATA_SEL 0x23

typedef enum {
    TASK_READY,
    TASK_RUNNING,
    TASK_SLEEPING,
    TASK_ZOMBIE
} task_state;

struct context_frame {
    unsigned int edi, esi, ebp, esp_dummy, ebx, edx, ecx, eax; 

    unsigned int int_no; 
    unsigned int err_code; 

    unsigned int eip, cs, eflags, useresp, ss; 
};

struct task_struct {
    unsigned int esp;           
    unsigned int pid;     
    task_state state;       
    void* stack_base;     
    unsigned int* page_directory; 
    struct task_struct* next;    
};

extern struct tss_entry_struct tss_entry;

void task_init();
struct task_struct* create_task(void (*entry_point)());
void schedule();
int init_scheduler();
unsigned int* vmm_create_address_space();

extern struct task_struct* volatile current_task;
extern struct task_struct* volatile task_list_head;
extern unsigned int next_pid;

#endif