[bits 32]

; ---------------------------------------------------------------------------
; Device interrupt stubs (vectors 0x30–0xEF). Each pushes the vector and
; dispatches it — MSI and MSI-X share these, since both deliver a LAPIC
; message carrying the vector.
; ---------------------------------------------------------------------------
extern msidev_dispatch
extern irq_apic_eoi

section .text

%macro msidev_entry 1
global msidev_stub_%1
msidev_stub_%1:
    push dword %1
    jmp msidev_common_dispatch
%endmacro

%assign msidev_vec 0x30
%rep 192
msidev_entry msidev_vec
%assign msidev_vec msidev_vec+1
%endrep

global msidev_common_dispatch
msidev_common_dispatch:
    pusha
    push ds
    push es
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov esi, [esp + 40]
    push esi
    call msidev_dispatch
    add esp, 4
    call irq_apic_eoi
    pop es
    pop ds
    popa
    add esp, 4
    iretd

section .rodata
global msidev_stub_table
msidev_stub_table:
%assign msidev_j 0x30
%rep 192
    dd msidev_stub_%+msidev_j
%assign msidev_j msidev_j+1
%endrep
