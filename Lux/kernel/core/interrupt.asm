[bits 32]

global keyboard_isr
global isr_common_stub

extern keyboard_handler
extern exception_handler

keyboard_isr:
    pusha
    call keyboard_handler
    popa
    iretd

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
    ; add esp, 8   
    iretd