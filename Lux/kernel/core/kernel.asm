[bits 32]
[global _start]

; Multiboot header
MODULEALIGN equ  1<<0
MEMINFO     equ  1<<1
VIDINFO     equ  1<<2
FLAGS       equ  MODULEALIGN | MEMINFO | VIDINFO
MAGIC       equ  0x1BADB002
CHECKSUM    equ -(MAGIC + FLAGS)

section .multiboot
align 4
    dd MAGIC
    dd FLAGS
    dd CHECKSUM
    dd 0, 0, 0, 0, 0 ; unused fields for a.out kludge
    dd 0 ; mode_type (0 = linear graphics)
    dd 1024 ; width
    dd 768 ; height
    dd 32 ; depth

section .bss
align 16
stack_bottom:
    resb 16384 ; 16 KB
stack_top:

section .text
extern init

_start:
    mov esp, stack_top
    
    ; Reset EFLAGS
    push 0
    popf

    ; Push multiboot info
    push ebx
    push eax

    call init
halt:
    cli
    hlt
    jmp halt