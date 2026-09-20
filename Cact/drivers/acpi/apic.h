#ifndef CACT_APIC_H
#define CACT_APIC_H

#include <stdint.h>
#include <stdbool.h>

int  apic_init(void);
bool apic_is_enabled(void);
/* Diagnostic: dump the LAPIC bits that gate local delivery, with a verdict
 * for the usual "armed but no interrupt" culprits. */
void apic_dump_state(const char *tag);
/* Diagnostic: one-line live LAPIC snapshot for the boot watchdog timeline. */
void apic_probe(const char *tag, unsigned ms, unsigned ticks);
/* Retire all in-service LAPIC entries (loops: one EOI clears only the highest
 * bit).  A stale entry holds PPR up and blocks the timer's priority class. */
void apic_clear_in_service(void);
void apic_eoi(void);
int  apic_pci_vector(uint8_t irq_pin);

uint32_t apic_lapic_base(void);
uint32_t apic_lapic_id(void);
bool     apic_lapic_ready(void);
bool     apic_x2apic_mode(void);
/* Why the LAPIC ended up in xAPIC or x2APIC mode (for logs and /proc/apic). */
const char *apic_x2apic_note(void);
/* MSI/MSI-X message address targeting this CPU; destination ID is 8-bit even
 * in x2APIC mode (see the definition for the interrupt-remapping caveat). */
uint32_t apic_msi_address(void);
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
