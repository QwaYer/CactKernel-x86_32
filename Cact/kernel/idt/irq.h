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

/* Called from interrupt.asm only */
void irq_dispatch(unsigned int irq);

#endif
