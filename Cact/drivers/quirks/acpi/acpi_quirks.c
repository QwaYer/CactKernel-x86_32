/* ACPI board quirks (manufacturer workarounds). */

#include "acpi_quirks.h"
#include "dmi.h"

uint32_t acpi_board_quirks(void)
{
    /* HP 290 G1 SFF (Intel B360, BIOS F.31): evaluating \_SB.PCI0._OSC
     * executes broken AML that references an unresolvable symbol
     * ([\_SB.PCI0._OSC.TBTS]) and trips ACPICA.  Skip _OSC only when SMBIOS
     * actually identifies the board as an HP 290 G1 — every other board
     * evaluates _OSC normally. */
    if (dmi_is_hp_290g1())
        return ACPI_QUIRK_DISABLE_OSC;
    return 0;
}
