/* xHCI — PCI glue: driver registration, interrupt dispatch, MSI-X setup. */

#include "xhci.h"
#include "xhci_internal.h"
#include "xhci_quirks.h"
#include "usb.h"
#include "pci_enum.h"
#include "pci_driver.h"
#include "pcidev.h"
#include "pci.h"
#include "kernel.h"
#include "memory.h"
#include "klib.h"
#include "sync.h"
#include "msi.h"

usb_hc_t *xhci_hc_list = NULL;
irq_spinlock_t xhci_evt_lock;

void xhci_irq_handler(void) {
    for (usb_hc_t *hc = xhci_hc_list; hc; hc = hc->irq_next)
        xhci_handle_irq(hc);
}

static int xhci_pci_probe(pci_device_t *pdev) {
    if (pdev->prog_if != 0x30) return -1;
    if (pdev->drv_probe_state >= 2) return 0;   /* already probed — no double probe */

    uint32_t mmio = 0;
    for (int i = 0; i < 6; i++) {
        pci_bar_t *bar = &pdev->bars[i];
        if (!bar->is_io && bar->base) {
            /* 64-bit BAR decoded above 4 GiB cannot be reached by the
             * 32-bit identity MMIO map — refuse instead of truncating. */
            if (bar->base >= 0x100000000ULL) {
                pr_warn("  %-11s : BAR%d above 4 GiB (0x%llx) not supported\n",
                        "xhci", i, (unsigned long long)bar->base);
                return -1;
            }
            mmio = (uint32_t)bar->base;
            break;
        }
    }
    if (!mmio) { pr_warn("  %-11s : MMIO BAR missing\n", "xhci"); return -1; }

    /* Memory space + bus mastering on.  Legacy INTx is disabled for good:
     * this driver delivers interrupts through MSI-X and nothing else. */
    pcidev_enable_mmio(pdev);
    pcidev_enable_bus_master(pdev);
    pcidev_disable_intx(pdev);

    uint32_t quirks = xhci_quirks_for(pdev);
    if (quirks & XHCI_QUIRK_SPURIOUS_REBOOT)
        pr_info("  %-11s : spurious-reboot quirk active\n", "xhci");

    volatile struct msix_table_entry *table = NULL;
    uint32_t table_size = 0;
    int cap_off = pci_msix_support(pdev);
    if (!cap_off || pci_msix_table_map(pdev, &table, &table_size) != 0 || !table_size) {
        pr_err("  %-11s : MSI-X unavailable (cap_off=%d) — controller not supported\n",
               "xhci", (int)cap_off);
        return -1;
    }

    int vec = msix_alloc_vector();
    if (vec <= 0) {
        pr_err("  %-11s : no free MSI-X vector\n", "xhci");
        return -1;
    }

    msix_register_handler(vec, xhci_irq_handler);
    pci_msix_enable(pdev, vec, table, 0);
    /* MSI-X enable/vector line is reported once by pci_msix_enable(). */

    if (xhci_init_one(mmio, quirks) < 0) {
        msix_unregister_handler(vec);
        msix_free_vector(vec);
        return -1;
    }
    return 0;
}

static pci_driver_t xhci_pci_driver = {
    .name       = "xhci_hcd",
    .vendor_id  = PCI_ANY_ID,
    .device_id  = PCI_ANY_ID,
    .class_code = 0x0C,
    .subclass   = 0x03,
    .probe      = xhci_pci_probe,
};

void xhci_pci_init(void) {
    irq_spinlock_init(&xhci_evt_lock);
    pci_register_driver(&xhci_pci_driver);

    /* Probing is deferred by design: pci_enum queues every device and
     * pcidev_probe_all() attaches this driver from the bootstrap task, once
     * interrupts and the scheduler are live.  Nothing to probe here — an
     * early scan only races the deferred queue and prints false negatives. */
}
