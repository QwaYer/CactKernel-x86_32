#include "task.h"
#include "kernel.h"
#include "memory.h"
#include "page_fault.h"
#include "libc.h"
#include "gdt.h"

struct task_struct* volatile current_task   = 0;
struct task_struct* volatile task_list_head = 0;
uint32_t            next_pid                = 1;
spinlock_t          scheduler_lock;

sched_queue_t ready_queue;
sched_queue_t sleep_queue;
sched_queue_t zombie_queue;

extern void vmm_fork_address_space(uint32_t* src_pd, uint32_t* dst_pd);


static sched_queue_t* queue_for_state(task_state s) {
    switch (s) {
        case TASK_READY:    return &ready_queue;
        case TASK_SLEEPING: return &sleep_queue;
        case TASK_ZOMBIE:   return &zombie_queue;
        default:            return 0;
    }
}

void task_set_state(struct task_struct* t, task_state old_state, task_state new_state) {
    if (!t || old_state == new_state) return;
    sched_queue_t* old_q = queue_for_state(old_state);
    if (old_q) sched_queue_remove(old_q, t);
    t->state = new_state;
    sched_queue_t* new_q = queue_for_state(new_state);
    if (new_q) sched_queue_push(new_q, t);
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

static void task_list_remove(struct task_struct* t) {
    if (!task_list_head || !t) return;
    if (task_list_head == t) {
        if (t->next == t) { task_list_head = 0; return; }
        task_list_head = t->next;
    }
    struct task_struct* cur = task_list_head;
    while (cur->next != task_list_head && cur->next != t) cur = cur->next;
    if (cur->next == t) cur->next = t->next;
    t->next = 0;
}


void task_init() {
    current_task   = 0;
    task_list_head = 0;
    next_pid       = 1;
    spinlock_init(&scheduler_lock);
    sched_queue_init(&ready_queue);
    sched_queue_init(&sleep_queue);
    sched_queue_init(&zombie_queue);
}


struct task_struct* create_task(void (*entry_point)()) {
    struct task_struct* t = (struct task_struct*)kmalloc(sizeof(struct task_struct));
    if (!t) return 0;

    uint32_t* stack = (uint32_t*)kalloc();
    if (!stack) { kfree_heap(t); return 0; }

    uint32_t* esp = (uint32_t*)((uint32_t)stack + 4096);

    *(--esp) = 0x00000202;          /* EFLAGS: IF=1             */
    *(--esp) = 0x08;                /* CS: kernel code segment  */
    *(--esp) = (uint32_t)entry_point;
    /* pusha: eax ecx edx ebx esp ebp esi edi */
    *(--esp) = 0; *(--esp) = 0; *(--esp) = 0; *(--esp) = 0;
    *(--esp) = 0; *(--esp) = 0; *(--esp) = 0; *(--esp) = 0;
    /* ds, es */
    *(--esp) = 0x10;
    *(--esp) = 0x10;

    t->esp            = (uint32_t)esp;
    t->stack_base     = (void*)stack;
    t->pid            = next_pid++;
    t->state          = TASK_READY;
    t->is_kernel      = 1;
    t->page_directory = 0;
    t->ustack_phys    = 0;
    t->ustack_virt    = 0;
    t->pending_signals= 0;
    t->queue_next     = 0;
    proc_tracker_init(&t->mm);

    spinlock_acquire(&scheduler_lock);
    task_list_add(t);
    sched_queue_push(&ready_queue, t);
    spinlock_release(&scheduler_lock);
    return t;
}


static struct task_struct* create_user_task_internal(void* entry_point, int add_to_list) {
    struct task_struct* t = (struct task_struct*)kmalloc(sizeof(struct task_struct));
    if (!t) return 0;

    uint32_t* kstack = (uint32_t*)kalloc();
    if (!kstack) { kfree_heap(t); return 0; }

    uint32_t* ustack_phys = (uint32_t*)kalloc();
    if (!ustack_phys) { kfree_page(kstack); kfree_heap(t); return 0; }

    uint32_t ustack_virt = 0xBFFFF000;
    t->ustack_phys = ustack_phys;
    t->ustack_virt = ustack_virt;

    uint32_t* esp = (uint32_t*)((uint32_t)kstack + 4096);

    /* iretd-фрейм для возврата в ring3 */
    *(--esp) = 0x23;                          /* SS                    */
    *(--esp) = ustack_virt + 4096 - 4;        /* ESP пользователя      */
    *(--esp) = 0x00000202;                    /* EFLAGS                */
    *(--esp) = 0x1B;                          /* CS ring3              */
    *(--esp) = (uint32_t)entry_point;         /* EIP                   */
    /* pusha */
    *(--esp) = 0; *(--esp) = 0; *(--esp) = 0; *(--esp) = 0;
    *(--esp) = 0; *(--esp) = 0; *(--esp) = 0;
    /* ds, es ring3 */
    *(--esp) = 0x23;
    *(--esp) = 0x23;

    t->esp             = (uint32_t)esp;
    t->stack_base      = (void*)kstack;
    t->pid             = next_pid++;
    t->state           = TASK_READY;
    t->is_kernel       = 0;
    t->page_directory  = 0;
    t->pending_signals = 0;
    t->queue_next      = 0;
    proc_tracker_init(&t->mm);
    for (int i = 0; i < MAX_FD; i++) t->fd_table[i] = 0;

    if (add_to_list) {
        spinlock_acquire(&scheduler_lock);
        task_list_add(t);
        sched_queue_push(&ready_queue, t);
        spinlock_release(&scheduler_lock);
    }
    return t;
}

struct task_struct* create_user_task(void* entry_point) {
    return create_user_task_internal(entry_point, 1);
}


struct task_struct* create_elf_task(char* path) {
    __asm__ __volatile__("cli");

    uint32_t* pd = vmm_create_address_space();
    if (!pd) { __asm__ __volatile__("sti"); return 0; }

    struct task_struct* t = create_user_task_internal((void*)0, 0);
    if (!t) { kfree_page(pd); __asm__ __volatile__("sti"); return 0; }

    proc_tracker_init(&t->mm);
    void* entry_point = load_elf(path, pd, &t->mm);
    if (!entry_point) {
        kprint("ELF: load failed\n");
        kfree_page(t->stack_base);
        kfree_page(t->ustack_phys);
        kfree_heap(t);
        kfree_page(pd);
        __asm__ __volatile__("sti");
        return 0;
    }

    t->page_directory = pd;

    uint32_t* stk = (uint32_t*)t->esp;
    stk[10] = (uint32_t)entry_point;

    vmm_map(pd, t->ustack_virt, (uint32_t)t->ustack_phys,
            PAGE_USER | PAGE_RW | PAGE_PRESENT);

    tss_entry.esp0 = (uint32_t)t->stack_base + 4096;

    spinlock_acquire(&scheduler_lock);
    task_list_add(t);
    sched_queue_push(&ready_queue, t);
    spinlock_release(&scheduler_lock);

    __asm__ __volatile__("sti");
    return t;
}


int init_scheduler() {
    current_task = (struct task_struct*)kmalloc(sizeof(struct task_struct));
    if (!current_task) return -1;

    current_task->pid             = 0;
    current_task->stack_base      = 0;
    current_task->state           = TASK_RUNNING;
    current_task->is_kernel       = 1;
    current_task->page_directory  = 0;
    current_task->ustack_phys     = 0;
    current_task->ustack_virt     = 0;
    current_task->next            = current_task;
    current_task->pending_signals = 0;
    current_task->queue_next      = 0;
    proc_tracker_init(&current_task->mm);
    task_list_head = current_task;

    create_task(terminal_task);
    return 0;
}


static void task_reap_internal() {
    struct task_struct* t;
    while ((t = sched_queue_pop(&zombie_queue)) != 0) {
        task_list_remove(t);
        proc_free_pages(&t->mm);
        if (t->stack_base)  kfree_page(t->stack_base);
        if (t->ustack_phys) kfree_page(t->ustack_phys);
        if (t->page_directory) vmm_free_address_space(t->page_directory);
        kfree_heap(t);
    }
}

void task_reap() {
    spinlock_acquire(&scheduler_lock);
    task_reap_internal();
    spinlock_release(&scheduler_lock);
}

void schedule() {
    spinlock_acquire(&scheduler_lock);

    if (!task_list_head || !current_task) {
        spinlock_release(&scheduler_lock);
        return;
    }

    task_reap_internal();
    task_handle_signals(current_task);

    if (current_task->state == TASK_ZOMBIE) {
        sched_queue_push(&zombie_queue, current_task);
        struct task_struct* next = sched_queue_pop(&ready_queue);
        if (!next) { spinlock_release(&scheduler_lock); return; }
        current_task = next;
        current_task->state = TASK_RUNNING;
        goto do_switch;
    }

    if (current_task->state == TASK_SLEEPING) {
        sched_queue_push(&sleep_queue, current_task);
        struct task_struct* next = sched_queue_pop(&ready_queue);
        if (!next) { spinlock_release(&scheduler_lock); return; }
        current_task = next;
        current_task->state = TASK_RUNNING;
        goto do_switch;
    }

    {
        struct task_struct* next = sched_queue_pop(&ready_queue);
        if (!next) { spinlock_release(&scheduler_lock); return; }
        current_task->state = TASK_READY;
        sched_queue_push(&ready_queue, current_task);
        current_task = next;
        current_task->state = TASK_RUNNING;
    }

do_switch:
    if (current_task->page_directory)
        switch_paging(current_task->page_directory);
    else {
        extern uint32_t page_directory[1024];
        switch_paging(page_directory);
    }
    tss_entry.esp0 = (uint32_t)current_task->stack_base + 4096;
    spinlock_release(&scheduler_lock);
}


void list_tasks() {
    spinlock_acquire(&scheduler_lock);
    if (!task_list_head) { spinlock_release(&scheduler_lock); return; }

    int count = 0;
    struct task_struct* tmp = task_list_head;
    do { count++; tmp = tmp->next; } while (tmp != task_list_head && count < 256);

    struct { uint32_t pid; task_state state; } tasks[count];
    tmp = task_list_head;
    for (int i = 0; i < count; i++) {
        tasks[i].pid   = tmp->pid;
        tasks[i].state = tmp->state;
        tmp = tmp->next;
    }
    spinlock_release(&scheduler_lock);

    kprint("\nPID  STATE\n");
    for (int i = 0; i < count; i++) {
        char buf[16];
        itoa((int)tasks[i].pid, buf);
        kprint(buf);
        switch (tasks[i].state) {
            case TASK_RUNNING:  kprint("  RUNNING\n");  break;
            case TASK_READY:    kprint("  READY\n");    break;
            case TASK_ZOMBIE:   kprint("  ZOMBIE\n");   break;
            default:            kprint("  SLEEPING\n"); break;
        }
    }
}

struct task_struct* task_fork(struct context_frame* regs) {
    __asm__ __volatile__("cli");

    struct task_struct* parent = current_task;
    if (!parent || parent->is_kernel) {
        __asm__ __volatile__("sti");
        return 0;
    }

    /* 1. Ядерный стек дочернего */
    uint32_t* kstack = (uint32_t*)kalloc();
    if (!kstack) { __asm__ __volatile__("sti"); return 0; }

    /* 2. Адресное пространство: CoW-форк */
    uint32_t* pd = vmm_create_address_space();
    if (!pd) {
        kfree_page(kstack);
        __asm__ __volatile__("sti");
        return 0;
    }
    vmm_fork_address_space(parent->page_directory, pd);

    uint32_t ustack_virt = parent->ustack_virt;
    vmm_map(pd, ustack_virt, (uint32_t)parent->ustack_phys,
            PAGE_USER | PAGE_COW | PAGE_PRESENT);

    struct task_struct* child = (struct task_struct*)kmalloc(sizeof(struct task_struct));
    if (!child) {
        kfree_page(kstack);
        vmm_free_address_space(pd);
        __asm__ __volatile__("sti");
        return 0;
    }

    child->pid             = next_pid++;
    child->state           = TASK_READY;
    child->is_kernel       = 0;
    child->stack_base      = kstack;
    child->ustack_phys     = parent->ustack_phys; /* CoW — своя страница при записи */
    child->ustack_virt     = ustack_virt;
    child->page_directory  = pd;
    child->pending_signals = 0;
    child->queue_next      = 0;

    proc_tracker_init(&child->mm);
    child->mm.page_dir = pd;

    for (int i = 0; i < MAX_FD; i++)
        child->fd_table[i] = parent->fd_table[i];

    /* 5. Ядерный стек дочернего: копирует прерванный контекст.
     *    Порядок полей ТОЧНО совпадает с context_frame из task.h:
     *    es, ds, edi, esi, ebp, esp_dummy, ebx, edx, ecx, eax,
     *    int_no, err_code, eip, cs, eflags, useresp, ss
     *    Но нужна только пользовательская часть (без int_no/err_code):  */
    uint32_t* esp = (uint32_t*)((uint32_t)kstack + PAGE_SIZE);

    /* iretd-фрейм (ring3 → ring0 → ring3) */
    *(--esp) = regs->ss;
    *(--esp) = regs->useresp;
    *(--esp) = regs->eflags;
    *(--esp) = regs->cs;
    *(--esp) = regs->eip;
    /* pusha: eax=0 (fork возвращает 0 в дочернем), остальное копируем */
    *(--esp) = 0;               /* EAX = 0 */
    *(--esp) = regs->ecx;
    *(--esp) = regs->edx;
    *(--esp) = regs->ebx;
    *(--esp) = 0;               /* esp_dummy */
    *(--esp) = regs->ebp;
    *(--esp) = regs->esi;
    *(--esp) = regs->edi;
    /* ds, es (порядок: в context_frame — es первый (ниже), ds второй) */
    *(--esp) = regs->ds;
    *(--esp) = regs->es;

    child->esp = (uint32_t)esp;

    spinlock_acquire(&scheduler_lock);
    task_list_add(child);
    sched_queue_push(&ready_queue, child);
    spinlock_release(&scheduler_lock);

    __asm__ __volatile__("sti");
    return child;
}


#ifndef KERNEL_BASE
#define KERNEL_BASE 0xC0000000U
#endif

int task_exec(char* path, struct context_frame* regs) {
    __asm__ __volatile__("cli");
    (void)regs;

    if (!path || (uint32_t)path >= KERNEL_BASE) {
        __asm__ __volatile__("sti");
        return -1;
    }

    struct task_struct* t = current_task;
    if (!t || t->is_kernel) { __asm__ __volatile__("sti"); return -1; }

    uint32_t* new_pd = vmm_create_address_space();
    if (!new_pd) { __asm__ __volatile__("sti"); return -1; }

    proc_page_tracker_t new_mm;
    proc_tracker_init(&new_mm);
    void* entry = load_elf(path, new_pd, &new_mm);
    if (!entry) {
        kfree_page(new_pd);
        __asm__ __volatile__("sti");
        return -1;
    }

    if (t->page_directory) vmm_free_address_space(t->page_directory);
    proc_free_pages(&t->mm);
    t->mm = new_mm;

    /* Новый стек пользователя */
    uint32_t* ustack_phys = (uint32_t*)kalloc();
    if (!ustack_phys) {
        proc_free_pages(&t->mm);
        __asm__ __volatile__("sti");
        return -1;
    }
    memory_set(ustack_phys, 0, PAGE_SIZE);

    uint32_t ustack_virt = 0xBFFFF000;
    vmm_map(new_pd, ustack_virt, (uint32_t)ustack_phys,
            PAGE_USER | PAGE_RW | PAGE_PRESENT);

    t->page_directory = new_pd;
    t->ustack_phys    = ustack_phys;
    t->ustack_virt    = ustack_virt;
    for (int i = 3; i < MAX_FD; i++) t->fd_table[i] = 0;

    /* Пересобрать ядерный стек для iretd в новый образ */
    uint32_t* esp = (uint32_t*)((uint32_t)t->stack_base + PAGE_SIZE);
    *(--esp) = 0x23;
    *(--esp) = ustack_virt + PAGE_SIZE - 4;
    *(--esp) = 0x00000202;
    *(--esp) = 0x1B;
    *(--esp) = (uint32_t)entry;
    *(--esp) = 0; *(--esp) = 0; *(--esp) = 0; *(--esp) = 0;
    *(--esp) = 0; *(--esp) = 0; *(--esp) = 0;
    *(--esp) = 0x23;
    *(--esp) = 0x23;
    t->esp = (uint32_t)esp;

    switch_paging(new_pd);
    tss_entry.esp0 = (uint32_t)t->stack_base + PAGE_SIZE;

    __asm__ __volatile__("sti");
    return 0;
}


void task_signal(uint32_t pid, uint32_t signal) {
    spinlock_acquire(&scheduler_lock);
    if (!task_list_head) { spinlock_release(&scheduler_lock); return; }
    struct task_struct* tmp = task_list_head;
    do {
        if (tmp->pid == pid && !tmp->is_kernel) {
            tmp->pending_signals |= signal;
            spinlock_release(&scheduler_lock);
            return;
        }
        tmp = tmp->next;
    } while (tmp != task_list_head);
    spinlock_release(&scheduler_lock);
}

void task_handle_signals(struct task_struct* t) {
    if (!t || t->is_kernel || !t->pending_signals) return;

    if (t->pending_signals & SIGKILL) {
        t->pending_signals = 0;
        t->state = TASK_ZOMBIE;
        return;
    }
    if (t->pending_signals & SIGTERM) {
        t->pending_signals &= ~SIGTERM;
        t->state = TASK_ZOMBIE;
        return;
    }
    if (t->pending_signals & SIGSTOP) {
        t->pending_signals &= ~SIGSTOP;
        t->state = TASK_SLEEPING;
        return;
    }
    if (t->pending_signals & SIGCONT) {
        t->pending_signals &= ~SIGCONT;
        if (t->state == TASK_SLEEPING) {
            sched_queue_remove(&sleep_queue, t);
            t->state = TASK_READY;
            sched_queue_push(&ready_queue, t);
        }
        return;
    }
}

void task_kill(uint32_t pid) {
    task_signal(pid, SIGKILL);
}