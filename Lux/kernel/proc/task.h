#ifndef TASK_H
#define TASK_H

#include <stdint.h>
#include "gdt.h"

#define USER_CODE_SEL 0x1B
#define USER_DATA_SEL 0x23

typedef enum {
    TASK_READY,
    TASK_RUNNING,
    TASK_SLEEPING,
    TASK_ZOMBIE
} task_state;

struct context_frame {
    uint32_t edi, esi, ebp, esp_dummy, ebx, edx, ecx, eax; 

    uint32_t int_no; 
    uint32_t err_code; 

    uint32_t eip, cs, eflags, useresp, ss; 
};

struct task_struct {
    uint32_t esp;           
    uint32_t pid;     
    task_state state;       
    void* stack_base;     
    uint32_t* page_directory; 
    struct task_struct* next;    
};

void task_init();
struct task_struct* create_task(void (*entry_point)());
struct task_struct* create_elf_task(char* path);
void schedule();
int init_scheduler();
uint32_t* vmm_create_address_space();

extern struct task_struct* volatile current_task;
extern struct task_struct* volatile task_list_head;
extern uint32_t next_pid;

extern void terminal_task();
extern void switch_to(uint32_t* old_esp, uint32_t new_esp);

#endif