[bits 32]

global timer_isr
global xhci_isr
global acpi_sci_isr
global pci_isr
global spurious_apic_isr
global ipi_halt_isr
global ipi_wake_isr

extern acpi_sci_callback
extern on_timer_tick
extern timer_tick
extern irq_apic_eoi
extern xhci_irq_handler
extern energy_ipi_halt_handle
extern energy_ipi_wake_handle

section .text

timer_isr:
    pusha
    push ds
    push es
    mov ax, 0x10
    mov ds, ax
    mov es, ax

    call timer_tick

    call irq_apic_eoi

    call on_timer_tick

    pop es
    pop ds
    popa
    iretd

xhci_isr:
    pusha
    push ds
    push es
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    call xhci_irq_handler
    call irq_apic_eoi
    pop es
    pop ds
    popa
    iretd

acpi_sci_isr:
    pusha
    push ds
    push es
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    call acpi_sci_callback
    call irq_apic_eoi
    pop es
    pop ds
    popa
    iretd

; ---------------------------------------------------------------------------
; Generic PCI INTx ISR (vectors 0xF0+). Just EOIs.
; ---------------------------------------------------------------------------
pci_isr:
    pusha
    push ds
    push es
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    call irq_apic_eoi
    pop es
    pop ds
    popa
    iretd

; ---------------------------------------------------------------------------
; APIC spurious interrupt handler — must be valid but does nothing (APIC
; doesn't expect EOI for spurious vectors).
; ---------------------------------------------------------------------------
spurious_apic_isr:
    iretd

; ---------------------------------------------------------------------------
; Energy C-state controller IPIs (vectors 0xF8/0xF9): master -> worker halt /
; wakeup protocol. Dispatch handlers live in the Rust energy/cstate modules.
; ---------------------------------------------------------------------------
ipi_halt_isr:
    pusha
    push ds
    push es
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    call energy_ipi_halt_handle
    call irq_apic_eoi
    pop es
    pop ds
    popa
    iretd

ipi_wake_isr:
    pusha
    push ds
    push es
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    call energy_ipi_wake_handle
    call irq_apic_eoi
    pop es
    pop ds
    popa
    iretd
