[bits 16]
[org 0x7c00]

boot:
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7c00          

    mov [BOOT_DRIVE], dl

    mov si, msg
    call print_string

    call load_kernel

    jmp 0x7e00

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

load_kernel:
    mov ah, 0x00
    int 0x13

    mov ah, 0x02       
    mov al, 35         
    mov ch, 0        
    mov dh, 0          
    mov cl, 2              
    mov dl, [BOOT_DRIVE] 
    mov bx, 0x7e00        
    
    int 0x13           

    jc disk_error

    cmp al, 35
    jne disk_error
    
    ret

disk_error:
    mov si, err_msg
    call print_string
    jmp $                   

msg db 'Lumen Bootloader: Loading OS...', 13, 10, 0
err_msg db 'Disk Error!', 0
BOOT_DRIVE db 0

times 510-($-$$) db 0
dw 0xaa55