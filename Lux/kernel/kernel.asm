[bits 16]
global _start         

_start:               
    xor ax, ax
    mov ds, ax
    mov es, ax

    call clear_screen   
    mov si, msg_real_mode
    call print_string

    cli                     
    lgdt [gdt_descriptor]   

    mov eax, cr0
    or eax, 0x1             
    mov cr0, eax

    jmp CODE_SEG:init_pm

print_string:
    mov ah, 0x0e
.loop:
    lodsb
    or al, al
    jz .done
    int 0x10
    jmp .loop
.done:
    ret

clear_screen:
    mov ah, 0x06
    mov al, 0
    mov bh, 0x07
    mov cx, 0
    mov dx, 0x184f
    int 0x10
    mov ah, 0x02
    mov bh, 0
    mov dh, 0
    mov dl, 0
    int 0x10
    ret

[bits 32]

global port_byte_out
global port_byte_in
extern init

kernel_entry:
    call init
    jmp $

port_byte_out:
    mov dx, [esp + 4]  
    mov al, [esp + 8]   
    out dx, al          
    ret

port_byte_in:
    mov dx, [esp + 4]   
    in al, dx            
    ret

init_pm:
    mov ax, DATA_SEG
    mov ds, ax
    mov ss, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    mov ebp, 0x90000
    mov esp, ebp

    [extern init] 
    call init

halt:
    jmp halt         

msg_real_mode db 'Jumping to 32-bit mode...', 0

gdt_start:
    dq 0x0          

gdt_code: 
    dw 0xffff       
    dw 0x0          
    db 0x0          
    db 10011010b    
    db 11001111b    
    db 0x0          

gdt_data:
    dw 0xffff
    dw 0x0
    db 0x0
    db 10010010b    
    db 11001111b
    db 0x0
gdt_end:

gdt_descriptor:
    dw gdt_end - gdt_start - 1
    dd gdt_start

CODE_SEG equ gdt_code - gdt_start
DATA_SEG equ gdt_data - gdt_start
