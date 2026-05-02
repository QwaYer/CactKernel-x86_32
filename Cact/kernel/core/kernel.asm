[bits 32]
; boot.asm — Multiboot2 entry point

[global _start]

; Multiboot2 header
MB2_MAGIC    equ 0xE85250D6
MB2_ARCH     equ 0                              ; i386 protected mode
MB2_LENGTH   equ header_end - header_start
MB2_CHECKSUM equ -(MB2_MAGIC + MB2_ARCH + MB2_LENGTH)

section .multiboot
align 8
header_start:
    dd MB2_MAGIC
    dd MB2_ARCH
    dd MB2_LENGTH
    dd MB2_CHECKSUM

    ; Information request tag
    align 8
ireq_start:
    dw 1                                         ; type = information request
    dw 0                                         ; flags
    dd ireq_end - ireq_start                     ; size
    dd 4                                         ; BASIC_MEMINFO
    dd 6                                         ; MMAP
    dd 8                                         ; FRAMEBUFFER
ireq_end:

    ; Framebuffer tag: ask for 1024x768x32
    align 8
fbtag_start:
    dw 5                                         ; type = framebuffer
    dw 0                                         ; flags (0 = required)
    dd fbtag_end - fbtag_start                   ; size
    dd 1024                                      ; width
    dd 768                                       ; height
    dd 32                                        ; bpp
fbtag_end:

    ; End tag
    align 8
    dw 0
    dw 0
    dd 8
header_end:

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

    ; Push multiboot2 info pointer (ebx) and magic (eax)
    push ebx
    push eax

    call init
halt:
    cli
    hlt
    jmp halt