#ifndef TASK_H
#define TASK_H

#include "memory.h"

// Состояния процесса
typedef enum {
    TASK_READY,      // Готов к выполнению
    TASK_RUNNING,    // Выполняется в данный момент
    TASK_SLEEPING,   // Ожидает (таймер или событие)
    TASK_ZOMBIE      // Завершен, но не удален из таблицы
} task_state;

// Структура для сохранения регистров (Контекст)
// Порядок важен! Он соответствует инструкции pushad и обработке прерываний
struct context_frame {
    // Регистры, сохраняемые вручную (pushad)
    unsigned int edi, esi, ebp, esp_dummy, ebx, edx, ecx, eax;
    
    // Регистры, сохраняемые процессором автоматически при прерывании
    unsigned int eip;
    unsigned int cs;
    unsigned int eflags;
};

// PCB (Process Control Block)
struct task_struct {
    unsigned int esp;            // Текущий указатель стека (состояние паузы)
    unsigned int pid;            // Идентификатор процесса
    task_state state;            // Статус
    void* stack_base;            // Базовый адрес стека (для kfree)
    
    struct task_struct* next;    // Ссылка на следующий процесс (для планировщика)
};

// Прототипы функций планировщика
void task_init();
struct task_struct* create_task(void (*entry_point)());
void schedule(); // Функция переключения задач

#endif