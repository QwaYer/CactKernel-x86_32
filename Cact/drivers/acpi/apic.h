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

void apic_ap_online(void);
void apic_send_init_ipi(uint32_t dest_lapic);
void apic_send_sipi(uint32_t dest_lapic, uint32_t vector);

/* Last-resort scheduler tick: program the legacy 8254 channel 0 at 100 Hz and
 * route its GSI to the timer ISR.  Used by the boot watchdog when the LAPIC
 * timer produces no ticks. */
void apic_pit_timer_fallback_enable(void);

#endif
