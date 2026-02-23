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

global timer_ticks_get

section .data
timer_ticks dd 0

section .text

timer_ticks_get:
    mov eax, [timer_ticks]
    ret


isr0:
    push dword 0   
    push dword 0   
    jmp isr_common_stub

isr13:
    push dword 13 
    jmp isr_common_stub

isr14:
    push dword 14 
    jmp isr_common_stub

; порядок на стеке (от младших адресов к старшим):
;   [esp+0]  = es
;   [esp+4]  = ds
;   [esp+8]  = edi  \
;   [esp+12] = esi   |
;   [esp+16] = ebp   |
;   [esp+20] = esp   | pusha
;   [esp+24] = ebx   |
;   [esp+28] = edx   |
;   [esp+32] = ecx   |
;   [esp+36] = eax  /
;   [esp+40] = int_no
;   [esp+44] = err_code
;   [esp+48] = eip    \  CPU
;   [esp+52] = cs      |
;   [esp+56] = eflags /
;   [esp+60] = useresp \  только при ring3→ring0
;   [esp+64] = ss      /
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

    inc dword [timer_ticks]  

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

    mov eax, [current_task]
    test eax, eax
    jz .no_switch

    mov [eax], esp
    call schedule
    mov eax, [current_task]
    mov esp, [eax]

.no_switch:
    pop es
    pop ds
    popa
    iretd