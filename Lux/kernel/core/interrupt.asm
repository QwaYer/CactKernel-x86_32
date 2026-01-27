[bits 32]

; Экспорт функций для использования в C
global keyboard_isr
global timer_isr
global isr_common_stub

; Импорт переменных и функций из ядра
extern keyboard_handler
extern exception_handler
extern current_task
extern schedule

; --- Обработчик прерывания Таймера (IRQ 0) ---
timer_isr:
    pusha                ; Сохраняем все регистры текущей задачи

    ; Загружаем сегмент данных ядра
    mov ax, 0x10
    mov ds, ax
    mov es, ax

    ; Проверяем, существует ли текущая задача (current_task != 0)
    mov eax, [current_task]
    test eax, eax        ; Аналог cmp eax, 0, но быстрее
    jz .skip_save        ; Если 0 (задач еще нет), пропускаем сохранение ESP

    ; Сохраняем текущий указатель стека (ESP) в структуру task_struct
    ; Предполагается, что 'unsigned int esp' — первое поле в структуре (offset 0)
    mov [eax], esp       

.skip_save:
    call schedule        ; Вызываем планировщик, он обновит current_task

    ; Загружаем ESP новой задачи, которую выбрал планировщик
    mov eax, [current_task]
    test eax, eax
    jz .do_eoi           ; Если задач нет, просто выходим
    mov esp, [eax]       ; Переключаем стек на стек новой задачи

.do_eoi:
    ; Отправляем EOI (End of Interrupt) контроллеру прерываний (PIC)
    mov al, 0x20
    out 0x20, al

    popa                 ; Восстанавливаем регистры новой задачи
    iretd                ; Прыгаем в код новой задачи

; --- Обработчик клавиатуры (IRQ 1) ---
keyboard_isr:
    pusha
    mov ax, 0x10
    mov ds, ax
    mov es, ax

    call keyboard_handler

    mov al, 0x20
    out 0x20, al
    popa
    iretd

; --- Общий обработчик исключений (Exceptions) ---
isr_common_stub:
    pusha        
    
    mov ax, ds    
    push eax            

    mov ax, 0x10        
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    call exception_handler

    pop eax             
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    popa           
    iretd