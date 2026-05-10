#include "irq.h"
#include "idt.h"
#include <stddef.h>

#define IRQ_CHAIN_MAX 4u

static void (*irq_handlers[16][IRQ_CHAIN_MAX])(void);
static unsigned char irq_chain_count[16];

void irq_dispatch(unsigned int irq) {
    if (irq >= 16) return;
    for (unsigned i = 0; i < (unsigned)irq_chain_count[irq]; i++) {
        void (*h)(void) = irq_handlers[irq][i];
        if (h) h();
    }
}

extern uint32_t irq_stub_table[];

void irq_register_handler(unsigned int irq, void (*handler)(void)) {
    if (irq >= 16) return;
    irq_chain_count[irq] = handler ? 1u : 0u;
    irq_handlers[irq][0] = handler;
    for (unsigned i = 1; i < IRQ_CHAIN_MAX; i++)
        irq_handlers[irq][i] = NULL;
    if (handler)
        set_idt_gate(KERNEL_IRQ_VECTOR_BASE + irq, irq_stub_table[irq]);
}

void irq_register_shared_handler(unsigned int irq, void (*handler)(void)) {
    if (irq >= 16 || !handler) return;
    for (unsigned i = 0; i < (unsigned)irq_chain_count[irq]; i++)
        if (irq_handlers[irq][i] == handler) return;
    if ((unsigned)irq_chain_count[irq] >= IRQ_CHAIN_MAX) return;
    if (irq_chain_count[irq] == 0)
        set_idt_gate(KERNEL_IRQ_VECTOR_BASE + irq, irq_stub_table[irq]);
    irq_handlers[irq][irq_chain_count[irq]++] = handler;
}

void irq_unregister_shared_handler(unsigned int irq, void (*handler)(void)) {
    if (irq >= 16 || !handler) return;
    for (unsigned i = 0; i < (unsigned)irq_chain_count[irq]; i++) {
        if (irq_handlers[irq][i] != handler) continue;
        for (unsigned j = i + 1; j < (unsigned)irq_chain_count[irq]; j++)
            irq_handlers[irq][j - 1] = irq_handlers[irq][j];
        irq_chain_count[irq]--;
        irq_handlers[irq][irq_chain_count[irq]] = NULL;
        return;
    }
}

unsigned irq_shared_handler_count(unsigned int irq) {
    if (irq >= 16) return 0;
    return (unsigned)irq_chain_count[irq];
}
