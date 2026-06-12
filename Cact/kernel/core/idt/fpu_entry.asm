[bits 32]

global isr7_nm_stub
global clear_xmm_regs
extern handle_lazy_fpu

isr7_nm_stub:
    pusha
    push ds
    push es

    mov ax, 0x10
    mov ds, ax
    mov es, ax

    call handle_lazy_fpu

    pop es
    pop ds
    popa
    iretd

clear_xmm_regs:
    pxor xmm0, xmm0
    pxor xmm1, xmm1
    pxor xmm2, xmm2
    pxor xmm3, xmm3
    pxor xmm4, xmm4
    pxor xmm5, xmm5
    pxor xmm6, xmm6
    pxor xmm7, xmm7
    ret
