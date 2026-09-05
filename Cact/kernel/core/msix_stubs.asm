[bits 32]

; ---------------------------------------------------------------------------
; MSI-X stubs (vectors 0x30–0xEF). Each pushes the vector and dispatches.
; ---------------------------------------------------------------------------
extern msix_dispatch
extern irq_apic_eoi

section .text

%macro msix_entry 1
global msix_stub_%1
msix_stub_%1:
    push dword %1
    jmp msix_common_dispatch
%endmacro

%assign msix_vec 0x30
%rep 192
msix_entry msix_vec
%assign msix_vec msix_vec+1
%endrep

global msix_common_dispatch
msix_common_dispatch:
    pusha
    push ds
    push es
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov esi, [esp + 40]
    push esi
    call msix_dispatch
    add esp, 4
    call irq_apic_eoi
    pop es
    pop ds
    popa
    add esp, 4
    iretd

section .rodata
global msix_stub_table
msix_stub_table:
%assign msix_j 0x30
%rep 192
    dd msix_stub_%+msix_j
%assign msix_j msix_j+1
%endrep
