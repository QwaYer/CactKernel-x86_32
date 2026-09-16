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
bool     apic_lapic_ready(void);
bool     apic_x2apic_mode(void);
/* LAPIC register access by xAPIC byte offset; MSR-addressed in x2APIC mode. */
uint32_t apic_lapic_read(uint32_t reg);
void     apic_lapic_write(uint32_t reg, uint32_t val);
int      apic_send_ipi(uint32_t dest_lapic, uint32_t vector);
bool     apic_ioapic_info(uint32_t *base, uint32_t *id, uint32_t *max_redir, uint32_t *gsi_base);
int      apic_irq_override(int isa_irq);

void apic_ap_online(void);
void apic_send_init_ipi(uint32_t dest_lapic);
void apic_send_sipi(uint32_t dest_lapic, uint32_t vector);

#endif
