[bits 16]
[org 0x7c00]

boot:
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7c00
    mov [BOOT_DRIVE], dl

    mov ah, 0x00
    int 0x13            

    mov ah, 0x02
    mov al, 32          
    mov ch, 0
    mov dh, 0
    mov cl, 2           
    mov dl, [BOOT_DRIVE]
    mov bx, 0x7e00
    int 0x13
    jc disk_error

    cmp al, 32
    jne disk_error

    mov dl, [BOOT_DRIVE]
    jmp 0x0000:0x7e00   

disk_error:
    mov si, err_msg
    call print_string
    jmp $

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

err_msg db 'Stage1 Disk Error!', 0
BOOT_DRIVE db 0

times 510-($-$$) db 0
dw 0xaa55