#ifndef CACT_QUIRKS_ACPI_H
#define CACT_QUIRKS_ACPI_H

#include <stdint.h>

/*
 * ACPI quirks — manufacturer workarounds for broken firmware AML/tables.
 */
#define ACPI_QUIRK_DISABLE_OSC   (1u << 0)   /* HP 290 G1 (BIOS F.31): PCI0._OSC method references an unresolvable symbol (_OSC.TBTS) */

/* Quirk bitmask for the running board. */
uint32_t acpi_board_quirks(void);

#endif
