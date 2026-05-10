#include "irq.h"
#include "idt.h"

static void (*irq_handlers[16])(void);

void irq_dispatch(unsigned int irq) {
    if (irq < 16 && irq_handlers[irq])
        irq_handlers[irq]();
}

extern uint32_t irq_stub_table[];

void irq_register_handler(unsigned int irq, void (*handler)(void)) {
    if (irq >= 16) return;
    irq_handlers[irq] = handler;
    if (handler)
        set_idt_gate(KERNEL_IRQ_VECTOR_BASE + irq, irq_stub_table[irq]);
}
