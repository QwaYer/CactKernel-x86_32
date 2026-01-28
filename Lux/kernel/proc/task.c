#include "task.h"
#include "memory.h"
#include "kernel.h"

extern unsigned int page_directory[1024]; 
extern void terminal_task();

struct task_struct* volatile current_task = 0;
struct task_struct* volatile task_list_head = 0;
unsigned int next_pid = 1;

void task_init() {
    current_task = 0;
    task_list_head = 0;
    next_pid = 1;
}

int init_scheduler() {
    current_task = (struct task_struct*)kmalloc(sizeof(struct task_struct));
    current_task->pid = 0; 
    current_task->state = TASK_RUNNING;
    current_task->page_directory = page_directory;
    current_task->stack_base = 0; 
    
    current_task->next = current_task;
    task_list_head = current_task;

    create_task(terminal_task);

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

    new_task->page_directory = vmm_create_address_space();
    if (!new_task->page_directory) return 0;

    unsigned int* stack_ptr = (unsigned int*)((unsigned char*)stack + PAGE_SIZE);

    *(--stack_ptr) = 0x202;        
    *(--stack_ptr) = 0x08;         
    *(--stack_ptr) = (unsigned int)entry_point;

    for (int i = 0; i < 8; i++) {
        *(--stack_ptr) = 0;         
    }

    new_task->esp = (unsigned int)stack_ptr;

    struct task_struct* last = task_list_head;
    while (last->next != task_list_head) {
        last = last->next;
    }
    last->next = new_task;
    new_task->next = task_list_head;

    return new_task;
}

void schedule() {
    if (!current_task) return;

    struct task_struct* next = current_task->next;

    if (next->page_directory != current_task->page_directory) {
        load_page_directory(next->page_directory);
    }

    current_task = next;
}

void list_tasks() {
    struct task_struct* tmp = task_list_head;
    kprint("\nPID  STAT  PAGE_DIR    STACK_BASE  SPACE\n");
    kprint("---  ----  ----------  ----------  -----\n");

    if (!tmp) return;

    do {
        char buf[16];

        itoa(tmp->pid, buf);
        kprint(buf); kprint("    ");

        if (tmp->state == TASK_RUNNING) kprint("RUN   ");
        else kprint("RDY   ");

        kprint("0x");
        hex_to_ascii((unsigned int)tmp->page_directory, buf);
        kprint(buf); kprint("  ");

        kprint("0x");
        hex_to_ascii((unsigned int)tmp->stack_base, buf);
        kprint(buf); kprint("  ");

        if (tmp->pid == 0) kprint("KERNEL\n");
        else kprint("USER\n");

        tmp = tmp->next;
    } while (tmp != task_list_head);

    char mem_buf[16];
    unsigned int free_mem = get_free_heap_memory();
    kprint("\nFree Heap: ");
    itoa(free_mem / 1024, mem_buf);
    kprint(mem_buf); kprint(" KB\n");
}