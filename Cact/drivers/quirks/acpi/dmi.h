#ifndef CACT_DMI_H
#define CACT_DMI_H

/*
 * SMBIOS/DMI board identification for the board-quirk layer.
 *
 * Firmware-provided SMBIOS tables are the only reliable way to tell which
 * physical board we are running on; PCI/ACPI only identify chipsets and
 * AML blobs, not the product.  dmi_is_hp_290g1() returns 1 when the SMBIOS
 * system information (type 1) reports an HP "290 G1" desktop (the Intel
 * B360 SFF/microtower that ships the broken _OSC AML), and 0 otherwise —
 * including every board that does not expose SMBIOS at all.
 */
int dmi_is_hp_290g1(void);

/*
 * Raw SMBIOS identity (system vendor / product name / BIOS version) as the
 * firmware reported it, for /proc/dmi.  Each returns a NUL-terminated string
 * that lives for the kernel lifetime, or NULL when the firmware exposes no
 * SMBIOS.  The first call runs the one-shot probe, so no separate init step is
 * needed.
 */
const char *dmi_vendor_string(void);
const char *dmi_product_string(void);
const char *dmi_bios_string(void);

#endif
