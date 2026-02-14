#include "task.h"
#include "kernel.h"
#include "memory.h"
#include "libc.h"
#include "gdt.h"

struct task_struct* volatile current_task = 0;
struct task_struct* volatile task_list_head = 0;
uint32_t next_pid = 1;

void task_init() {
    current_task = 0;
    task_list_head = 0;
    next_pid = 1;
}

struct task_struct* create_task(void (*entry_point)()) {
    struct task_struct* new_task = (struct task_struct*)kmalloc(sizeof(struct task_struct));
    if (!new_task) return 0;

    uint32_t* stack = (uint32_t*)kalloc();
    if (!stack) return 0;

    uint32_t* esp = (uint32_t*)((uint32_t)stack + 4096);

    /* Порядок на стеке (от меньшего адреса к большему):
       es, ds, edi, esi, ebp, esp_dummy, ebx, edx, ecx, eax,  <- pusha
       eip, cs, eflags                                          <- CPU (без ss/useresp т.к. ring0)
    */

    /* iretd frame — то что CPU восстановит */
    *(--esp) = 0x10;                        /* SS (kernel data) — не используется в ring0 */
    *(--esp) = 0;                           /* useresp — не используется в ring0 */
    *(--esp) = 0x00000202;                  /* EFLAGS: IF=1, резервный бит=1 */
    *(--esp) = 0x08;                        /* CS (kernel code) */
    *(--esp) = (uint32_t)entry_point;   /* EIP */

    /* pusha frame */
    *(--esp) = 0; /* eax */
    *(--esp) = 0; /* ecx */
    *(--esp) = 0; /* edx */
    *(--esp) = 0; /* ebx */
    *(--esp) = 0; /* esp_dummy */
    *(--esp) = 0; /* ebp */
    *(--esp) = 0; /* esi */
    *(--esp) = 0; /* edi */

    /* push ds, push es */
    *(--esp) = 0x10; /* es */
    *(--esp) = 0x10; /* ds */

    new_task->esp = (uint32_t)esp;
    new_task->stack_base = (void*)stack;
    new_task->pid = next_pid++;
    new_task->state = TASK_READY;

    if (!task_list_head) {
        task_list_head = new_task;
        new_task->next = new_task;
    } else {
        struct task_struct* last = task_list_head;
        while (last->next != task_list_head) last = last->next;
        last->next = new_task;
        new_task->next = task_list_head;
    }

    return new_task;
}

struct task_struct* create_elf_task(char* path) {
    uint32_t* pd = vmm_create_address_space();
    if (!pd) return 0;

    void* entry_point = load_elf(path, pd);
    if (!entry_point) {
        /* Здесь нужно освободить pd, но пока упростим */
        return 0;
    }

    struct task_struct* t = create_task((void (*)())entry_point);
    if (t) {
        t->page_directory = pd;
    }
    return t;
}

int init_scheduler() {
    current_task = (struct task_struct*)kmalloc(sizeof(struct task_struct));
    if (!current_task) return -1;
    current_task->pid = 0;
    current_task->stack_base = 0;
    current_task->state = TASK_RUNNING;
    current_task->next = current_task;
    task_list_head = current_task;

    create_task(terminal_task);

    return 0;
}

void schedule() {
    if (!task_list_head || !current_task) return;

    struct task_struct* next = current_task->next;
    int checked = 0;
    while (next->state != TASK_READY && next->state != TASK_RUNNING) {
        next = next->next;
        if (++checked > 64) return; /* защита от зависания */
    }

    if (next == current_task) return; /* только одна задача */

    current_task->state = TASK_READY;
    current_task = next;
    current_task->state = TASK_RUNNING;

    if (current_task->page_directory) {
        switch_paging(current_task->page_directory);
    }

    tss_entry.esp0 = (uint32_t)current_task->stack_base + 4096;
}

void list_tasks() {
    if (!task_list_head) return;
    struct task_struct* tmp = task_list_head;
    kprint("\nPID  STATE\n");
    do {
        char buf[16];
        itoa(tmp->pid, buf);
        kprint(buf);
        kprint(tmp->state == TASK_RUNNING ? "  RUNNING\n" : "  READY\n");
        tmp = tmp->next;
    } while (tmp != task_list_head);
}