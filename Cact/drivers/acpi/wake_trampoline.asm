; S3-resume trampoline — staged by the kernel in conventional memory and
; advertised as the FACS waking vector.  After an S3 wake the platform resets
; the machine; the firmware performs an S3 resume and jumps here in real mode.
; We install a flat GDT, switch to protected mode, turn paging back on with the
; kernel page directory, and jump to the C resume entry.
;
; Layout (base fixed at 0xA000; the SMP AP trampoline owns 0x8000):
;   +0x0000  real-mode entry (equals the FACS waking vector)
;   +0x0F00  info_cr3    physical address of the kernel page directory
;   +0x0F04  info_stack  top of the resume stack (physical == linear)
;   +0x0F08  info_entry  acpi_resume_entry() (kernel text, identity mapped)

[bits 16]
org 0xA000

%define INFO_BASE 0xAF00

section .text

wake_start16:
    cli
    ; A20 may be off after the resume; turn it back on (fast gate) so the
    ; kernel image above 1 MB is reachable.
    in  al, 0x92
    or  al, 2
    out 0x92, al

    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0xA000 + 0x0E00

    lgdt [gdt_desc]

    mov eax, cr0
    or al, 1
    mov cr0, eax

    jmp dword 0x08:wake_start32

[bits 32]
wake_start32:
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

; Patch block, kept near the top of the page so a trampoline that grows into it
; turns into a build error rather than a silently overwritten value.
align 16
info_block:
    times (INFO_BASE - 0xA000 - ($ - $$)) db 0
info_cr3:    dd 0
info_stack:  dd 0
info_entry:  dd 0
