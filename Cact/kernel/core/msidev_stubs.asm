[bits 32]

; ---------------------------------------------------------------------------
; Device interrupt stubs (vectors 0x30–0xEF). Each pushes an error-code slot
; and the vector, then dispatches — MSI and MSI-X share these, since both
; deliver a LAPIC message carrying the vector.
;
; The error-code slot is a zero: device interrupts have none, but pushing it
; makes the frame the stubs leave on the stack exactly a `struct context_frame`
; (es, ds, pusha, int_no, err_code, eip, cs, eflags[, useresp, ss]), so the
; dispatcher can publish the interrupted context as-is — see get_irq_regs().
; ---------------------------------------------------------------------------
extern msidev_dispatch
extern irq_apic_eoi

section .text

%macro msidev_entry 1
global msidev_stub_%1
msidev_stub_%1:
    push dword 0
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

    mov eax, esp            ; the entry frame is a struct context_frame
    mov esi, [eax + 40]     ; ... whose int_no is the vector the stub pushed
    push eax                ; msidev_dispatch(vector, frame)
    push esi
    call msidev_dispatch
    add esp, 8

    call irq_apic_eoi
    pop es
    pop ds
    popa
    add esp, 8              ; drop the vector and the error-code slot
    iretd

section .rodata
global msidev_stub_table
msidev_stub_table:
%assign msidev_j 0x30
%rep 192
    dd msidev_stub_%+msidev_j
%assign msidev_j msidev_j+1
%endrep
