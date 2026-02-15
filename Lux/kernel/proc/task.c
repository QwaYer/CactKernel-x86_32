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

static void task_list_add(struct task_struct* t) {
    if (!task_list_head) {
        task_list_head = t;
        t->next = t;
    } else {
        struct task_struct* last = task_list_head;
        while (last->next != task_list_head) last = last->next;
        last->next = t;
        t->next = task_list_head;
    }
}

struct task_struct* create_task(void (*entry_point)()) {
    struct task_struct* t = (struct task_struct*)kmalloc(sizeof(struct task_struct));
    if (!t) return 0;

    uint32_t* stack = (uint32_t*)kalloc();
    if (!stack) { kfree_heap(t); return 0; }

    uint32_t* esp = (uint32_t*)((uint32_t)stack + 4096);

    *(--esp) = 0x00000202;
    *(--esp) = 0x08;
    *(--esp) = (uint32_t)entry_point;
    *(--esp) = 0; *(--esp) = 0; *(--esp) = 0; *(--esp) = 0;
    *(--esp) = 0; *(--esp) = 0; *(--esp) = 0; *(--esp) = 0;
    *(--esp) = 0x10;
    *(--esp) = 0x10;

    t->esp = (uint32_t)esp;
    t->stack_base = (void*)stack;
    t->pid = next_pid++;
    t->state = TASK_READY;
    t->is_kernel = 1;
    t->page_directory = 0;
    t->ustack_phys = 0;
    t->ustack_virt = 0;

    task_list_add(t);
    return t;
}

struct task_struct* create_user_task(void* entry_point) {
    struct task_struct* t = (struct task_struct*)kmalloc(sizeof(struct task_struct));
    if (!t) return 0;

    uint32_t* kstack = (uint32_t*)kalloc();
    if (!kstack) { kfree_heap(t); return 0; }

    uint32_t* ustack_phys = (uint32_t*)kalloc();
    if (!ustack_phys) { kfree_heap(t); return 0; }

    uint32_t ustack_virt = 0xBFFFF000;

    t->ustack_phys = ustack_phys;
    t->ustack_virt = ustack_virt;

    uint32_t* esp = (uint32_t*)((uint32_t)kstack + 4096);

    *(--esp) = 0x23;
    *(--esp) = ustack_virt + 4096 - 4;
    *(--esp) = 0x00000202;
    *(--esp) = 0x1B;
    *(--esp) = (uint32_t)entry_point;
    *(--esp) = 0; *(--esp) = 0; *(--esp) = 0; *(--esp) = 0;
    *(--esp) = 0; *(--esp) = 0; *(--esp) = 0; *(--esp) = 0;
    *(--esp) = 0x23;
    *(--esp) = 0x23;

    t->esp = (uint32_t)esp;
    t->stack_base = (void*)kstack;
    t->pid = next_pid++;
    t->state = TASK_READY;
    t->is_kernel = 0;
    t->page_directory = 0;

    task_list_add(t);
    return t;
}

struct task_struct* create_elf_task(char* path) {
    uint32_t* pd = vmm_create_address_space();
    if (!pd) return 0;

    void* entry_point = load_elf(path, pd);
    if (!entry_point) {
        kprint("ELF: load failed\n");
        return 0;
    }

    struct task_struct* t = create_user_task(entry_point);
    if (!t) return 0;

    t->page_directory = pd;

    vmm_map(pd,
            t->ustack_virt,
            (uint32_t)t->ustack_phys,
            PAGE_USER | PAGE_RW | PAGE_PRESENT);

    tss_entry.esp0 = (uint32_t)t->stack_base + 4096;

    return t;
}

int init_scheduler() {
    current_task = (struct task_struct*)kmalloc(sizeof(struct task_struct));
    if (!current_task) return -1;
    current_task->pid = 0;
    current_task->stack_base = 0;
    current_task->state = TASK_RUNNING;
    current_task->is_kernel = 1;
    current_task->page_directory = 0;
    current_task->ustack_phys = 0;
    current_task->ustack_virt = 0;
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
        if (++checked > 64) return;
    }

    if (next == current_task) return;

    current_task->state = TASK_READY;
    current_task = next;
    current_task->state = TASK_RUNNING;

    if (current_task->page_directory)
        switch_paging(current_task->page_directory);

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
        if      (tmp->state == TASK_RUNNING) kprint("  RUNNING\n");
        else if (tmp->state == TASK_READY)   kprint("  READY\n");
        else if (tmp->state == TASK_ZOMBIE)  kprint("  ZOMBIE\n");
        else                                 kprint("  SLEEPING\n");
        tmp = tmp->next;
    } while (tmp != task_list_head);
}