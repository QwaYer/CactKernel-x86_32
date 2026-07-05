#include "idt.h"
#include "kernel.h"
#include "memory.h"
#include "klib.h"

// Exception handlers (ISRs 0-31)
extern void isr0();  extern void isr1();  extern void isr2();
extern void isr3();  extern void isr4();  extern void isr5();
extern void isr6();  extern void isr7_nm_stub();  extern void isr8();
extern void isr9();  extern void isr10(); extern void isr11();
extern void isr12(); extern void isr13(); extern void isr14();
extern void isr15(); extern void isr16(); extern void isr17();
extern void isr18(); extern void isr19(); extern void isr20();
extern void isr21(); extern void isr22(); extern void isr23();
extern void isr24(); extern void isr25(); extern void isr26();
extern void isr27(); extern void isr28(); extern void isr29();
extern void isr30(); extern void isr31();

// IRQ handlers
extern void timer_isr();
extern void usb_isr();
extern void uhci_isr();
extern void ohci_isr();
extern void pci_isr();

static struct idt_entry idt[256];
static struct idt_ptr   idtp;

// Set a single IDT gate (interrupt handler)
void set_idt_gate(int n, uint32_t handler) {
    idt[n].low_offset  = (uint16_t)(handler & 0xFFFF);
    idt[n].sel         = 0x08;           // Kernel code segment
    idt[n].always0     = 0;
    idt[n].flags       = 0x8E;           // Present, ring0, 32-bit interrupt gate
    idt[n].high_offset = (uint16_t)((handler >> 16) & 0xFFFF);
}

// Initialize IDT and load it
int init_idt(void) {
    idtp.limit = (sizeof(struct idt_entry) * 256) - 1;
    idtp.base  = (uint32_t)&idt;
    memory_set(&idt, 0, sizeof(struct idt_entry) * 256);

    // Install CPU exception gates (0-31)
    set_idt_gate(0,  (uint32_t)isr0);
    set_idt_gate(1,  (uint32_t)isr1);
    set_idt_gate(2,  (uint32_t)isr2);
    set_idt_gate(3,  (uint32_t)isr3);
    set_idt_gate(4,  (uint32_t)isr4);
    set_idt_gate(5,  (uint32_t)isr5);
    set_idt_gate(6,  (uint32_t)isr6);
    set_idt_gate(7,  (uint32_t)isr7_nm_stub);
    set_idt_gate(8,  (uint32_t)isr8);
    set_idt_gate(9,  (uint32_t)isr9);
    set_idt_gate(10, (uint32_t)isr10);
    set_idt_gate(11, (uint32_t)isr11);
    set_idt_gate(12, (uint32_t)isr12);
    set_idt_gate(13, (uint32_t)isr13);
    set_idt_gate(14, (uint32_t)isr14);
    set_idt_gate(15, (uint32_t)isr15);
    set_idt_gate(16, (uint32_t)isr16);
    set_idt_gate(17, (uint32_t)isr17);
    set_idt_gate(18, (uint32_t)isr18);
    set_idt_gate(19, (uint32_t)isr19);
    set_idt_gate(20, (uint32_t)isr20);
    set_idt_gate(21, (uint32_t)isr21);
    set_idt_gate(22, (uint32_t)isr22);
    set_idt_gate(23, (uint32_t)isr23);
    set_idt_gate(24, (uint32_t)isr24);
    set_idt_gate(25, (uint32_t)isr25);
    set_idt_gate(26, (uint32_t)isr26);
    set_idt_gate(27, (uint32_t)isr27);
    set_idt_gate(28, (uint32_t)isr28);
    set_idt_gate(29, (uint32_t)isr29);
    set_idt_gate(30, (uint32_t)isr30);
    set_idt_gate(31, (uint32_t)isr31);

    // Install IRQ handlers (hardware interrupts)
    set_idt_gate(0x20, (uint32_t)timer_isr);        // IRQ0  - timer
    // ISA IRQ vectors 1-15: safe stubs (IOAPIC still routes here, any
    // stray interrupt must have a valid gate to avoid #GP).
    for (int i = 1; i < 16; i++)
        set_idt_gate(0x20 + i, (uint32_t)pci_isr);
    // ACPI SCI vector set dynamically by AcpiOsInstallInterruptHandler.

    // IOAPIC PCI INTx vectors (GSI 16+ mapped to 0xF0+)
    set_idt_gate(0xF0, (uint32_t)pci_isr);
    set_idt_gate(0xF1, (uint32_t)pci_isr);
    set_idt_gate(0xF2, (uint32_t)pci_isr);
    set_idt_gate(0xF3, (uint32_t)pci_isr);
    set_idt_gate(0xF4, (uint32_t)pci_isr);
    set_idt_gate(0xF5, (uint32_t)pci_isr);
    set_idt_gate(0xF6, (uint32_t)pci_isr);
    set_idt_gate(0xF7, (uint32_t)pci_isr);
    // Reserved PCI range 0xF8-0xFE + APIC spurious vector 0xFF
    for (int i = 0xF8; i < 0xFF; i++)
        set_idt_gate(i, (uint32_t)pci_isr);
    extern void spurious_apic_isr();
    set_idt_gate(0xFF, (uint32_t)spurious_apic_isr);

    // Load IDT into IDTR
    __asm__ __volatile__("lidt (%0)" : : "r"(&idtp));
    return 0;
}