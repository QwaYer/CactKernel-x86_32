[bits 16]
[org 0x7e00]

stage2_start:
    mov [BOOT_DRIVE], dl    

    xor ax, ax
    mov ds, ax
    mov es, ax

    mov si, msg_loading
    call print_string

    mov ah, 0x00
    mov dl, [BOOT_DRIVE]
    int 0x13

    mov ax, 0x1000
    mov es, ax
    xor bx, bx

    mov word  [dap_count],   127
    mov dword [dap_lba_lo],  33
    mov dword [dap_lba_hi],  0
    mov word  [dap_seg],     0x1000
    mov word  [dap_off],     0x0000

    mov ah, 0x42
    mov si, dap
    mov dl, [BOOT_DRIVE]
    int 0x13
    jc disk_error

    mov word  [dap_count],   64
    mov dword [dap_lba_lo],  160
    mov dword [dap_lba_hi],  0
    mov word  [dap_seg],     0x1FE0
    mov word  [dap_off],     0x0000

    mov ah, 0x42
    mov si, dap
    mov dl, [BOOT_DRIVE]
    int 0x13
    jc disk_error

    mov si, msg_pm
    call print_string

    cli
    lgdt [gdt_descriptor]

    mov eax, cr0
    or eax, 1
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

disk_error:
    mov si, err_msg
    call print_string
    jmp $

dap:
    db  0x10        
    db  0x00       
dap_count:
    dw  0           
dap_off:
    dw  0         
dap_seg:
    dw  0          
dap_lba_lo:
    dd  0         
dap_lba_hi:
    dd  0          

msg_loading db 'Stage2: Loading kernel...', 13, 10, 0
msg_pm      db 'Entering protected mode...', 13, 10, 0
err_msg     db 'Stage2 Disk Error!', 13, 10, 0
BOOT_DRIVE  db 0


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


[bits 32]

init_pm:
    mov ax, DATA_SEG
    mov ds, ax
    mov ss, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    mov ebp, 0x90000
    mov esp, ebp

    jmp 0x10000             


times 16384-($-$$) db 0