#ifndef CACT_APIC_H
#define CACT_APIC_H

#include <stdint.h>
#include <stdbool.h>

int  apic_init(void);
bool apic_is_enabled(void);
void apic_eoi(void);

#endif
