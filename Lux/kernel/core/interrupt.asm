[bits 32]

global keyboard_isr
global virtio_net_isr
global timer_isr
global syscall_isr
global isr_common_stub
global isr0
global isr1
global isr2
global isr3
global isr4
global isr5
global isr6
global isr7
global isr8
global isr9
global isr10
global isr11
global isr12
global isr13
global isr14
global isr15
global isr16
global isr17
global isr18
global isr19
global isr20
global isr21
global isr22
global isr23
global isr24
global isr25
global isr26
global isr27
global isr28
global isr29
global isr30
global isr31

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

isr1:
    push dword 0
    push dword 1
    jmp isr_common_stub

isr2:
    push dword 0
    push dword 2
    jmp isr_common_stub

isr3:
    push dword 0
    push dword 3
    jmp isr_common_stub

isr4:
    push dword 0
    push dword 4
    jmp isr_common_stub

isr5:
    push dword 0
    push dword 5
    jmp isr_common_stub

isr6:
    push dword 0
    push dword 6
    jmp isr_common_stub

isr7:
    push dword 0
    push dword 7
    jmp isr_common_stub

isr8:
    push dword 8
    jmp isr_common_stub

isr9:
    push dword 0
    push dword 9
    jmp isr_common_stub

isr10:
    push dword 10
    jmp isr_common_stub

isr11:
    push dword 11
    jmp isr_common_stub

isr12:
    push dword 12
    jmp isr_common_stub

isr13:
    push dword 13
    jmp isr_common_stub

isr14:
    push dword 14
    jmp isr_common_stub

isr15:
    push dword 0
    push dword 15
    jmp isr_common_stub

isr16:
    push dword 0
    push dword 16
    jmp isr_common_stub

isr17:
    push dword 17
    jmp isr_common_stub

isr18:
    push dword 0
    push dword 18
    jmp isr_common_stub

isr19:
    push dword 0
    push dword 19
    jmp isr_common_stub

isr20:
    push dword 0
    push dword 20
    jmp isr_common_stub

isr21:
    push dword 21
    jmp isr_common_stub

isr22:
    push dword 0
    push dword 22
    jmp isr_common_stub

isr23:
    push dword 0
    push dword 23
    jmp isr_common_stub

isr24:
    push dword 0
    push dword 24
    jmp isr_common_stub

isr25:
    push dword 0
    push dword 25
    jmp isr_common_stub

isr26:
    push dword 0
    push dword 26
    jmp isr_common_stub

isr27:
    push dword 0
    push dword 27
    jmp isr_common_stub

isr28:
    push dword 0
    push dword 28
    jmp isr_common_stub

isr29:
    push dword 29
    jmp isr_common_stub

isr30:
    push dword 30
    jmp isr_common_stub

isr31:
    push dword 0
    push dword 31
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