#ifndef CACT_APIC_H
#define CACT_APIC_H

#include <stdint.h>
#include <stdbool.h>

int  apic_init(void);
bool apic_is_enabled(void);
void apic_eoi(void);
int  apic_pci_vector(uint8_t irq_pin);

uint32_t apic_lapic_base(void);
uint32_t apic_lapic_id(void);
volatile uint32_t *apic_lapic_regs(void);
bool     apic_ioapic_info(uint32_t *base, uint32_t *id, uint32_t *max_redir, uint32_t *gsi_base);
int      apic_irq_override(int isa_irq);

#endif
