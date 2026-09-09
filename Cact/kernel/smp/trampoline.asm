; AP trampoline — copied to low memory (0x8000) and executed by worker cores
; after an INIT-SIPI wake.  Runs in real mode, switches to protected mode with
; paging (kernel page directory), then jumps to the C entry smp_ap_entry.
;
; Layout (first 20 bytes are patched by the BSP before sending SIPIs):
;   +0  info_cr3     - physical address of the kernel page directory
;   +4  info_stack   - per-CPU idle stack top (physical == linear)
;   +8  info_entry   - smp_ap_entry() (kernel text, identity mapped)
;   +12 info_cpu     - logical cpu index this AP should take
;   +16 info_ack     - set by the AP to 1 after it consumed the info block

[bits 16]
org 0x8000

section .text

align 16
info_cr3:    dd 0
info_stack:  dd 0
info_entry:  dd 0
info_cpu:    dd 0
info_ack:    dd 0

align 16
ap_start16:
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x8000 + 0x0F00

    lgdt [gdt_desc]

    mov eax, cr0
    or al, 1
    mov cr0, eax

    jmp dword 0x08:ap_start32

[bits 32]
ap_start32:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax

    mov esp, [info_stack]

    mov eax, [info_cr3]
    mov cr3, eax

    mov eax, cr0
    or eax, 0x80000000
    mov cr0, eax

    mov eax, [info_entry]
    jmp eax

align 16
gdt_start:
    dq 0x0000000000000000
    dq 0x00CF9A000000FFFF        ; kernel code  0x08
    dq 0x00CF92000000FFFF        ; kernel data  0x10
gdt_end:

align 2
gdt_desc:
    dw gdt_end - gdt_start - 1
    dd gdt_start
