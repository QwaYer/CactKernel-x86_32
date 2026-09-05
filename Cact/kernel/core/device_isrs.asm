[bits 32]

global timer_isr
global xhci_isr
global acpi_sci_isr
global pci_isr
global spurious_apic_isr

extern acpi_sci_callback
extern on_timer_tick
extern acpi_pm_timer_tick
extern timer_eoi
extern irq_apic_eoi
extern xhci_irq_handler

section .text

timer_isr:
    pusha
    push ds
    push es
    mov ax, 0x10
    mov ds, ax
    mov es, ax

    call acpi_pm_timer_tick

    call timer_eoi

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
