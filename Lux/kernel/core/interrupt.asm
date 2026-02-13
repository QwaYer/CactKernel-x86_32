[bits 32]

global keyboard_isr
global timer_isr
global syscall_isr
global isr_common_stub
global isr0
global isr13
global isr14

extern keyboard_handler
extern exception_handler
extern syscall_handler
extern current_task
extern schedule

; Exception stubs
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

; Common exception handler stub
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

; Timer IRQ handler (IRQ0 -> IDT entry 32)
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
    mov al, 0x20
    out 0x20, al

    pop es
    pop ds
    popa
    iretd

; Keyboard IRQ handler (IRQ1 -> IDT entry 33)
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

; Syscall ISR
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

    pop es
    pop ds
    popa
    iretd