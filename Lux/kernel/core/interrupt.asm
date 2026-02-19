[bits 32]

global keyboard_isr
global virtio_net_isr
global timer_isr
global syscall_isr
global isr_common_stub
global isr0
global isr13
global isr14

extern keyboard_handler
extern virtio_net_irq_handler
extern exception_handler
extern syscall_handler
extern current_task
extern schedule

isr0:
    push 0
    push 0
    jmp isr_common_stub

isr13:
    push 13
    jmp isr_common_stub

isr14:
    push 14
    jmp isr_common_stub

isr_common_stub:
    pusha
    push ds
    push es
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    push esp
    call exception_handler
    add esp, 4
    pop es
    pop ds
    popa
    add esp, 8
    iretd

timer_isr:
    pusha
    push ds
    push es
    mov ax, 0x10
    mov ds, ax
    mov es, ax

    mov eax, [current_task]
    test eax, eax
    jz .skip_save
    mov [eax], esp

.skip_save:
    call schedule

    mov eax, [current_task]
    test eax, eax
    jz .do_eoi
    mov esp, [eax]

.do_eoi:
    ; send EOI to PIC
    mov al, 0x20
    out 0x20, al

    pop es
    pop ds
    popa
    iretd

keyboard_isr:
    pusha
    push ds
    push es
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    call keyboard_handler
    mov al, 0x20
    out 0x20, al
    pop es
    pop ds
    popa
    iretd

virtio_net_isr:
    pusha
    call virtio_net_irq_handler
    ; send EOI to both PICs
    mov al, 0x20
    out 0xA0, al
    out 0x20, al
    popa
    iret

syscall_isr:
    pusha
    push ds
    push es
    mov ax, 0x10
    mov ds, ax
    mov es, ax

    push esp
    call syscall_handler
    add esp, 4

    ; После системного вызова проверяем, не нужно ли переключить задачу
    ; (например, если процесс завершился или вызвал yield)
    mov eax, [current_task]
    test eax, eax
    jz .no_switch

    ; Сохраняем текущий стек в структуру задачи
    mov [eax], esp
    
    call schedule
    
    ; Загружаем стек новой (или той же) задачи
    mov eax, [current_task]
    mov esp, [eax]

.no_switch:
    pop es
    pop ds
    popa
    iretd