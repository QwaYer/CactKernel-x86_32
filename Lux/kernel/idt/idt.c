#include "idt.h"
#include "kernel.h"
#include "memory.h"
#include "libc.h"

extern void isr0();  extern void isr1();  extern void isr2();
extern void isr3();  extern void isr4();  extern void isr5();
extern void isr6();  extern void isr7();  extern void isr8();
extern void isr9();  extern void isr10(); extern void isr11();
extern void isr12(); extern void isr13(); extern void isr14();
extern void isr15(); extern void isr16(); extern void isr17();
extern void isr18(); extern void isr19(); extern void isr20();
extern void isr21(); extern void isr22(); extern void isr23();
extern void isr24(); extern void isr25(); extern void isr26();
extern void isr27(); extern void isr28(); extern void isr29();
extern void isr30(); extern void isr31();

extern void timer_isr();
extern void keyboard_isr();
extern void mouse_isr();
extern void nvme_isr();
extern void virtio_net_isr();
extern void syscall_isr();
extern void usb_isr();
extern void uhci_isr();
extern void ohci_isr();
extern void xhci_isr();
extern void spurious_irq7();
extern void spurious_irq15();

static struct idt_entry idt[256];
static struct idt_ptr   idtp;

void set_idt_gate(int n, uint32_t handler) {
    idt[n].low_offset  = (uint16_t)(handler & 0xFFFF);
    idt[n].sel         = 0x08;
    idt[n].always0     = 0;
    idt[n].flags       = 0x8E;
    idt[n].high_offset = (uint16_t)((handler >> 16) & 0xFFFF);
}

void init_pic(void) {
    kprint("[PIC] sending ICW1 — cascade mode, ICW4 needed\n");
    port_byte_out(0x20, 0x11);
    port_byte_out(0xA0, 0x11);

    kprint("[PIC] remapping: master IRQ0-7 → 0x20-0x27  slave IRQ8-15 → 0x28-0x2F\n");
    port_byte_out(0x21, 0x20);
    port_byte_out(0xA1, 0x28);

    port_byte_out(0x21, 0x04);  // master: slave on IRQ2
    port_byte_out(0xA1, 0x02);  // slave: cascade identity

    port_byte_out(0x21, 0x01);  // 8086 mode
    port_byte_out(0xA1, 0x01);

    port_byte_out(0x21, 0xF8);  // master: unmask IRQ0(timer), IRQ1(kbd), IRQ2(cascade)
    port_byte_out(0xA1, 0x80);  // slave: mask only IRQ15, rest open for USB/mouse/nvme
    kprint("[PIC] mask: master=0xF8 (IRQ0/1/2 active)  slave=0x80 (IRQ15 masked)\n");
    klog(LOG_OK, "PIC remapped and configured");
}

int init_idt(void) {
    idtp.limit = (sizeof(struct idt_entry) * 256) - 1;
    idtp.base  = (uint32_t)&idt;
    memory_set(&idt, 0, sizeof(struct idt_entry) * 256);

    kprint("[IDT] zeroed 256 entries  base=0x");
    char buf[16]; hex_to_ascii(idtp.base, buf); kprint(buf);
    kprint("  limit="); itoa(idtp.limit, buf); kprint(buf); kprint("\n");

    kprint("[IDT] installing exception gates ISR0-ISR31 (CPU exceptions)\n");
    set_idt_gate(0,  (uint32_t)isr0);
    set_idt_gate(1,  (uint32_t)isr1);
    set_idt_gate(2,  (uint32_t)isr2);
    set_idt_gate(3,  (uint32_t)isr3);
    set_idt_gate(4,  (uint32_t)isr4);
    set_idt_gate(5,  (uint32_t)isr5);
    set_idt_gate(6,  (uint32_t)isr6);
    set_idt_gate(7,  (uint32_t)isr7);
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

    kprint("[IDT] installing IRQ gates:"
           " 0x20=timer  0x21=kbd  0x2C=mouse  0x2D=net  0x2E=nvme"
           "  0x29-0x2B=usb\n");
    set_idt_gate(0x20, (uint32_t)timer_isr);        // IRQ0
    set_idt_gate(0x21, (uint32_t)keyboard_isr);      // IRQ1
    set_idt_gate(0x27, (uint32_t)spurious_irq7);     // IRQ7  — master spurious
    set_idt_gate(0x29, (uint32_t)usb_isr);            // IRQ9
    set_idt_gate(0x2A, (uint32_t)usb_isr);            // IRQ10
    set_idt_gate(0x2B, (uint32_t)usb_isr);            // IRQ11
    set_idt_gate(0x2C, (uint32_t)mouse_isr);          // IRQ12
    set_idt_gate(0x2D, (uint32_t)virtio_net_isr);     // IRQ13
    set_idt_gate(0x2E, (uint32_t)nvme_isr);           // IRQ14
    set_idt_gate(0x2F, (uint32_t)spurious_irq15);     // IRQ15 — slave spurious

    kprint("[IDT] installing syscall gate 0x80 (DPL=3, ring-3 accessible)\n");
    set_idt_gate(0x80, (uint32_t)syscall_isr);
    idt[0x80].flags = 0xEE;

    kprint("[IDT] loading IDTR\n");
    __asm__ __volatile__("lidt (%0)" : : "r"(&idtp));
    klog(LOG_OK, "IDT loaded — exceptions and IRQs armed");
    return 0;
}