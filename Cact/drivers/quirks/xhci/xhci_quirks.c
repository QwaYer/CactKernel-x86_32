/* xHCI quirks — per-device workaround table.
 *
 * First matching entry wins.  The trailing PCI_ANY_ID row is the default
 * policy applied to every controller that is not matched by a more specific
 * entry.  Interrupt delivery is MSI-X on every controller; the table only
 * refines reset/recovery behaviour.
 */

#include "xhci_quirks.h"
#include "pci_enum.h"
#include "pci_driver.h"

struct xhci_quirk_entry {
    uint16_t vendor_id;
    uint16_t device_id;      /* PCI_ANY_ID = wildcard */
    uint32_t quirks;
};

static const struct xhci_quirk_entry xhci_quirk_table[] = {
    /* Intel 300-series PCH (B360/H370/Z390) + Cannon Lake-LP: spurious HSE
     * trips a chipset reboot — mask it. */
    { 0x8086, 0xA36D, XHCI_QUIRK_INTEL_HOST | XHCI_QUIRK_SPURIOUS_REBOOT },
    { 0x8086, 0x9DED, XHCI_QUIRK_INTEL_HOST | XHCI_QUIRK_SPURIOUS_REBOOT },
    { 0x8086, 0x8CA2, XHCI_QUIRK_INTEL_HOST | XHCI_QUIRK_SPURIOUS_REBOOT },
    { PCI_ANY_ID, PCI_ANY_ID, 0 },
};

uint32_t xhci_quirks_for(pci_device_t *pdev)
{
    for (unsigned int i = 0;
         i < sizeof(xhci_quirk_table) / sizeof(xhci_quirk_table[0]); i++) {
        const struct xhci_quirk_entry *e = &xhci_quirk_table[i];
        if ((e->vendor_id == PCI_ANY_ID || e->vendor_id == pdev->vendor_id) &&
            (e->device_id == PCI_ANY_ID || e->device_id == pdev->device_id))
            return e->quirks;
    }
    return 0;
}
