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
    mov ax, ds
    push eax   
    mov ax, 0x10 
    mov ds, ax
    mov es, ax
    call exception_handler
    pop eax
    mov ds, ax
    mov es, ax
    popa
    add esp, 8  
    iretd

timer_isr:
    pusha              
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
    popa                 
    iretd

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

syscall_isr:
    pusha
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    push esp             
    call syscall_handler
    add esp, 4
    popa
    iretd