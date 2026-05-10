#ifndef IRQ_H
#define IRQ_H

#include <stdint.h>

#define KERNEL_IRQ_VECTOR_BASE 0x20

/*
 * Install a C callback for hardware IRQ 0..15. Reprograms IDT[0x20+irq] to a
 * stub that EOIs the PIC and invokes the handler. IRQ0 is the timer — avoid
 * replacing it unless you reproduce timer logic.
 */
void irq_register_handler(unsigned int irq, void (*handler)(void));

/* Append a handler for PIC IRQ lines shared by multiple PCI devices (INTx).
 * The primary module may use irq_register_handler first; others call this. */
void irq_register_shared_handler(unsigned int irq, void (*handler)(void));
void irq_unregister_shared_handler(unsigned int irq, void (*handler)(void));
unsigned irq_shared_handler_count(unsigned int irq);

/* Called from interrupt.asm only */
void irq_dispatch(unsigned int irq);

#endif
