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

    *(--esp) = 0x00000202; // EFLAGS
    *(--esp) = 0x08;       // CS
    *(--esp) = (uint32_t)entry_point; // EIP
    *(--esp) = 0;          // EAX
    *(--esp) = 0;          // ECX
    *(--esp) = 0;          // EDX
    *(--esp) = 0;          // EBX
    *(--esp) = 0;          // ESP (dummy)
    *(--esp) = 0;          // EBP
    *(--esp) = 0;          // ESI
    *(--esp) = 0;          // EDI
    *(--esp) = 0x10;       // DS
    *(--esp) = 0x10;       // ES

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

/*
 * Внутренняя версия create_user_task без task_list_add.
 * create_elf_task использует её, чтобы добавить задачу в список
 * только после того, как всё готово (страницы замаплены).
 */
static struct task_struct* create_user_task_internal(void* entry_point, int add_to_list) {
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

    *(--esp) = 0x23;                    // SS
    *(--esp) = ustack_virt + 4096 - 4; // ESP (вершина user stack)
    *(--esp) = 0x00000202;             // EFLAGS (IF=1)
    *(--esp) = 0x1B;                   // CS (ring3 code)
    *(--esp) = (uint32_t)entry_point;  // EIP
    *(--esp) = 0;                      // EAX
    *(--esp) = 0;                      // ECX
    *(--esp) = 0;                      // EDX
    *(--esp) = 0;                      // EBX
    *(--esp) = 0;                      // ESP (dummy для pusha)
    *(--esp) = 0;                      // EBP
    *(--esp) = 0;                      // ESI
    *(--esp) = 0;                      // EDI
    *(--esp) = 0x23;                   // DS
    *(--esp) = 0x23;                   // ES

    t->esp = (uint32_t)esp;
    t->stack_base = (void*)kstack;
    t->pid = next_pid++;
    t->state = TASK_READY;
    t->is_kernel = 0;
    t->page_directory = 0;

    if (add_to_list)
        task_list_add(t);

    return t;
}

struct task_struct* create_user_task(void* entry_point) {
    return create_user_task_internal(entry_point, 1);
}

struct task_struct* create_elf_task(char* path) {
    /* Отключаем прерывания на время подготовки задачи,
     * чтобы планировщик не запустил незавершённую задачу. */
    __asm__ __volatile__("cli");

    uint32_t* pd = vmm_create_address_space();
    if (!pd) {
        __asm__ __volatile__("sti");
        return 0;
    }

    void* entry_point = load_elf(path, pd);
    if (!entry_point) {
        kprint("ELF: load failed\n");
        __asm__ __volatile__("sti");
        return 0;
    }

    /* Создаём задачу БЕЗ добавления в список планировщика */
    struct task_struct* t = create_user_task_internal(entry_point, 0);
    if (!t) {
        __asm__ __volatile__("sti");
        return 0;
    }

    t->page_directory = pd;

    /* Маппим user stack в адресное пространство процесса */
    vmm_map(pd,
            t->ustack_virt,
            (uint32_t)t->ustack_phys,
            PAGE_USER | PAGE_RW | PAGE_PRESENT);

    tss_entry.esp0 = (uint32_t)t->stack_base + 4096;

    /* Только теперь задача полностью готова — добавляем в список */
    task_list_add(t);

    __asm__ __volatile__("sti");
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