[bits 32]
[global _start]

; Multiboot header
MODULEALIGN equ  1<<0
MEMINFO     equ  1<<1
FLAGS       equ  MODULEALIGN | MEMINFO
MAGIC       equ  0x1BADB002
CHECKSUM    equ -(MAGIC + FLAGS)

section .multiboot
align 4
    dd MAGIC
    dd FLAGS
    dd CHECKSUM

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