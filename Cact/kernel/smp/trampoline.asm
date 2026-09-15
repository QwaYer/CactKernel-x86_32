; AP trampoline — copied to low memory (0x8000) and executed by worker cores
; after an INIT-SIPI wake.  Runs in real mode, switches to protected mode with
; paging (kernel page directory), then jumps to the C entry smp_ap_entry.
;
; The SIPI vector is TRAMP_ADDR >> 12, so a woken core always lands on the first
; byte of this page and starts fetching instructions there.  Offset 0 must
; therefore be the entry instruction: the info block below is data, and an AP
; that begins executing it never reaches the entry code (the values patched
; here are addresses, and a byte pair like 0xF0 0xC3 decodes as `lock ret`).
; The block is parked at a fixed offset near the top of the page instead and is
; reached only through absolute addresses (org 0x8000).
;
;   +0x0000              AP entry (real mode)
;   +0x0F00 info_cr3     physical address of the kernel page directory
;   +0x0F04 info_stack   per-CPU idle stack top (physical == linear)
;   +0x0F08 info_entry   smp_ap_entry() (kernel text, identity mapped)
;   +0x0F0C info_cpu     logical cpu index this AP should take
;   +0x0F10 info_ack     set by the AP to 1 after it consumed the info block

[bits 16]
org 0x8000

; Must match the INFO_* offsets in proc/sched/src/smp.rs.
%define INFO_BASE 0x8F00

section .text

ap_start16:
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x8000 + 0x0E00

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

; Info block.  The fill keeps these words out of the instruction stream and
; turns a trampoline that grows into INFO_BASE into a build error instead of a
; silently overwritten (BSP-patched) value.
align 16
info_block:
    times (INFO_BASE - 0x8000 - ($ - $$)) db 0
info_cr3:    dd 0
info_stack:  dd 0
info_entry:  dd 0
info_cpu:    dd 0
info_ack:    dd 0
