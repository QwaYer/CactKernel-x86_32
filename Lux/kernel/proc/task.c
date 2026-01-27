#include "task.h"
#include "memory.h" 

struct task_struct* volatile current_task = 0;
struct task_struct* volatile task_list_head = 0;
unsigned int next_pid = 1;

void task_init() {
    current_task = 0;
    task_list_head = 0;
}

struct task_struct* create_task(void (*entry_point)()) {
    struct task_struct* new_task = (struct task_struct*)kmalloc(sizeof(struct task_struct));
    if (!new_task) return 0;

    void* stack = kalloc(); 
    if (!stack) {
        kfree_heap(new_task);
        return 0;
    }

    new_task->stack_base = stack;
    new_task->pid = next_pid++;
    new_task->state = TASK_READY;
    new_task->next = 0;

    // Стек в x86 растет вниз. Ставим указатель на самый верх страницы.
    unsigned int* stack_ptr = (unsigned int*)((unsigned char*)stack + PAGE_SIZE);

    // Имитируем состояние процессора при входе в прерывание
    *(--stack_ptr) = 0x0202;          // EFLAGS (с включенными прерываниями)
    *(--stack_ptr) = 0x08;            // CS (селектор кода в твоем GDT)
    *(--stack_ptr) = (unsigned int)entry_point; // EIP

    // Имитируем результат pusha (8 регистров: eax, ecx, edx, ebx, oesp, ebp, esi, edi)
    for (int i = 0; i < 8; i++) {
        *(--stack_ptr) = 0;
    }

    new_task->esp = (unsigned int)stack_ptr;

    // Добавляем в список задач (простой связный список)
    if (!task_list_head) {
        task_list_head = new_task;
        current_task = new_task; // Делаем первой задачей
    } else {
        struct task_struct* tmp = (struct task_struct*)task_list_head;
        while (tmp->next) tmp = tmp->next;
        tmp->next = new_task;
    }

    return new_task;
}

// Простейший алгоритм: Round Robin
void schedule() {
    if (!current_task) return;
    
    if (current_task->next) {
        current_task = current_task->next;
    } else {
        current_task = task_list_head; // Возвращаемся в начало очереди
    }
}