[bits 32]
[global _start]

extern init

_start:
    mov ax, 0x10        ; DATA_SEG
    mov ds, ax
    mov ss, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ebp, 0x90000
    mov esp, ebp
    call init
halt:
    jmp halt