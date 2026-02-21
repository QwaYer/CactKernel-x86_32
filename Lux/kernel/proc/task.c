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
    t->pending_signals = 0;
    proc_tracker_init(&t->mm);

    task_list_add(t);
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

    *(--esp) = 0x23;                    // SS
    *(--esp) = ustack_virt + 4096 - 4; // ESP (вершина user stack)
    *(--esp) = 0x00000202;             // EFLAGS (IF=1)
    *(--esp) = 0x1B;                   // CS (ring3 code)
    *(--esp) = (uint32_t)entry_point;  // EIP  ← для ELF перезапишем ниже
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
    t->pending_signals = 0;
    proc_tracker_init(&t->mm);
    for (int i = 0; i < MAX_FD; i++) t->fd_table[i] = 0;

    if (add_to_list)
        task_list_add(t);

    return t;
}

struct task_struct* create_user_task(void* entry_point) {
    return create_user_task_internal(entry_point, 1);
}

struct task_struct* create_elf_task(char* path) {
    __asm__ __volatile__("cli");

    uint32_t* pd = vmm_create_address_space();
    if (!pd) {
        __asm__ __volatile__("sti");
        return 0;
    }

    struct task_struct* t = create_user_task_internal((void*)0, 0);
    if (!t) {
        kfree_page(pd);
        __asm__ __volatile__("sti");
        return 0;
    }

    proc_tracker_init(&t->mm);

    void* entry_point = load_elf(path, pd, &t->mm);
    if (!entry_point) {
        kprint("ELF: load failed\n");
        /* proc_free_pages уже вызван внутри load_elf */
        kfree_page(t->stack_base);
        kfree_page(t->ustack_phys);
        kfree_heap(t);
        __asm__ __volatile__("sti");
        return 0;
    }

    t->page_directory = pd;

    /*
     * Исправляем EIP в уже подготовленном стеке.
     * Раскладка от вершины (t->esp): ES, DS, EDI, ESI, EBP, ESP_dummy,
     * EBX, EDX, ECX, EAX, EIP — итого 10 слов до EIP.
     */
    uint32_t* stk = (uint32_t*)t->esp;
    stk[10] = (uint32_t)entry_point;

    vmm_map(pd,
            t->ustack_virt,
            (uint32_t)t->ustack_phys,
            PAGE_USER | PAGE_RW | PAGE_PRESENT);

    tss_entry.esp0 = (uint32_t)t->stack_base + 4096;

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
    current_task->pending_signals = 0;
    proc_tracker_init(&current_task->mm);
    task_list_head = current_task;

    create_task(terminal_task);
    return 0;
}

void task_reap() {
    if (!task_list_head) return;

    struct task_struct* prev = task_list_head;
    struct task_struct* curr = task_list_head->next;
    int count = 0;

    while (curr != task_list_head && count < 64) {
        count++;
        if (curr->state == TASK_ZOMBIE && !curr->is_kernel) {
            prev->next = curr->next;

            /*
             * proc_free_pages освобождает:
             *   - все физические страницы кода/данных/bss (mm.pages[])
             *   - page directory через vmm_free_address_space
             * Поэтому отдельный вызов vmm_free_address_space не нужен.
             * ustack_phys управляется вручную (не через трекер).
             */
            proc_free_pages(&curr->mm);

            if (curr->stack_base)
                kfree_page(curr->stack_base);

            if (curr->ustack_phys)
                kfree_page(curr->ustack_phys);

            struct task_struct* dead = curr;
            curr = prev->next;
            kfree_heap(dead);
        } else {
            prev = curr;
            curr = curr->next;
        }
    }
}

void schedule() {
    if (!task_list_head || !current_task) return;

    task_reap();

    struct task_struct* next = current_task->next;
    int checked = 0;
    while (next->state != TASK_READY && next->state != TASK_RUNNING) {
        next = next->next;
        if (++checked > 64) return;
    }

    if (next == current_task) return;

    current_task->state = TASK_READY;
    current_task = next;

    task_handle_signals(current_task);

    if (current_task->state == TASK_ZOMBIE || current_task->state == TASK_SLEEPING) {
        return;
    }

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

/*
 * task_fork — создаёт копию текущего процесса.
 * Страницы форка выделяются напрямую через kalloc() и не проходят
 * через трекер родителя — дочерний процесс управляет ими сам через
 * vmm_free_address_space при завершении (task_reap → proc_free_pages).
 */
struct task_struct* task_fork(struct context_frame* regs) {
    __asm__ __volatile__("cli");

    struct task_struct* parent = current_task;
    if (!parent || parent->is_kernel) {
        __asm__ __volatile__("sti");
        return 0;
    }

    /* Новый kernel stack */
    uint32_t* kstack = (uint32_t*)kalloc();
    if (!kstack) { __asm__ __volatile__("sti"); return 0; }

    /* Новый user stack — копируем содержимое родительского */
    uint32_t* ustack_phys = (uint32_t*)kalloc();
    if (!ustack_phys) { kfree_page(kstack); __asm__ __volatile__("sti"); return 0; }
    memory_copy(ustack_phys, parent->ustack_phys, PAGE_SIZE);

    /* Новое адресное пространство */
    uint32_t* pd = vmm_create_address_space();
    if (!pd) {
        kfree_page(kstack);
        kfree_page(ustack_phys);
        __asm__ __volatile__("sti");
        return 0;
    }

    for (int i = 0; i < 1024; i++) {
        if (!(parent->page_directory[i] & PAGE_PRESENT)) continue;
        if (i < 32) { pd[i] = parent->page_directory[i]; continue; }

        uint32_t* src_pt = (uint32_t*)(parent->page_directory[i] & ~0xFFF);
        uint32_t* dst_pt = (uint32_t*)kalloc();
        if (!dst_pt) continue;
        for (int j = 0; j < 1024; j++) dst_pt[j] = 0;

        for (int j = 0; j < 1024; j++) {
            if (!(src_pt[j] & PAGE_PRESENT)) continue;
            void* new_page = kalloc();
            if (!new_page) continue;
            memory_copy(new_page, (void*)(src_pt[j] & ~0xFFF), PAGE_SIZE);
            dst_pt[j] = ((uint32_t)new_page & ~0xFFF) | (src_pt[j] & 0xFFF);
        }
        pd[i] = ((uint32_t)dst_pt) | (parent->page_directory[i] & 0xFFF);
    }

    uint32_t ustack_virt = parent->ustack_virt;
    vmm_map(pd, ustack_virt, (uint32_t)ustack_phys, PAGE_USER | PAGE_RW | PAGE_PRESENT);

    struct task_struct* child = (struct task_struct*)kmalloc(sizeof(struct task_struct));
    if (!child) {
        kfree_page(kstack);
        kfree_page(ustack_phys);
        __asm__ __volatile__("sti");
        return 0;
    }

    child->pid            = next_pid++;
    child->state          = TASK_READY;
    child->is_kernel      = 0;
    child->stack_base     = kstack;
    child->ustack_phys    = ustack_phys;
    child->ustack_virt    = ustack_virt;
    child->page_directory = pd;
    child->pending_signals = 0;

    proc_tracker_init(&child->mm);
    child->mm.page_dir = pd;   /* чтобы proc_free_pages освободил pd */

    for (int i = 0; i < MAX_FD; i++) child->fd_table[i] = parent->fd_table[i];

    uint32_t* esp = (uint32_t*)((uint32_t)kstack + PAGE_SIZE);

    *(--esp) = regs->ss;
    *(--esp) = regs->useresp;
    *(--esp) = regs->eflags;
    *(--esp) = regs->cs;
    *(--esp) = regs->eip;
    *(--esp) = 0;           /* EAX = 0 в дочернем */
    *(--esp) = regs->ecx;
    *(--esp) = regs->edx;
    *(--esp) = regs->ebx;
    *(--esp) = 0;           /* ESP dummy */
    *(--esp) = regs->ebp;
    *(--esp) = regs->esi;
    *(--esp) = regs->edi;
    *(--esp) = regs->ds;
    *(--esp) = regs->es;

    child->esp = (uint32_t)esp;

    tss_entry.esp0 = (uint32_t)child->stack_base + PAGE_SIZE;
    task_list_add(child);

    __asm__ __volatile__("sti");
    return child;
}

int task_exec(char* path, struct context_frame* regs) {
    __asm__ __volatile__("cli");

    struct task_struct* t = current_task;
    if (!t || t->is_kernel) { __asm__ __volatile__("sti"); return -1; }

    uint32_t* new_pd = vmm_create_address_space();
    if (!new_pd) { __asm__ __volatile__("sti"); return -1; }

    proc_tracker_init(&t->mm);

    void* entry = load_elf(path, new_pd, &t->mm);
    if (!entry) {
        /* proc_free_pages (включая new_pd) уже вызван внутри load_elf */
        __asm__ __volatile__("sti");
        return -1;
    }

    if (t->page_directory)
        vmm_free_address_space(t->page_directory);

    uint32_t* ustack_phys = (uint32_t*)kalloc();
    if (!ustack_phys) {
        proc_free_pages(&t->mm); 
        __asm__ __volatile__("sti");
        return -1;
    }
    memory_set(ustack_phys, 0, PAGE_SIZE);

    uint32_t ustack_virt = 0xBFFFF000;
    vmm_map(new_pd, ustack_virt, (uint32_t)ustack_phys, PAGE_USER | PAGE_RW | PAGE_PRESENT);

    t->page_directory = new_pd;
    t->ustack_phys    = ustack_phys;
    t->ustack_virt    = ustack_virt;

    for (int i = 3; i < MAX_FD; i++) t->fd_table[i] = 0;

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
    if (!task_list_head) return;
    struct task_struct* tmp = task_list_head;
    do {
        if (tmp->pid == pid && !tmp->is_kernel) {
            tmp->pending_signals |= signal;
            return;
        }
        tmp = tmp->next;
    } while (tmp != task_list_head);
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
        if (t->state == TASK_SLEEPING)
            t->state = TASK_READY;
        return;
    }
}

void task_kill(uint32_t pid) {
    task_signal(pid, SIGKILL);
}