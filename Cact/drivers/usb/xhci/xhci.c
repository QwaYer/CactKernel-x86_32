/* xHCI — PCI glue: driver registration, interrupt dispatch, MSI-X / INTx setup. */

#include "xhci.h"
#include "xhci_internal.h"
#include "usb.h"
#include "pci_enum.h"
#include "pci_driver.h"
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

    uint32_t mmio = 0;
    for (int i = 0; i < 6; i++) {
        if (!pdev->bars[i].is_io && pdev->bars[i].base) {
            mmio = pdev->bars[i].base; break;
        }
    }
    if (!mmio)
        mmio = pci_read_config_dword(pdev->bus, pdev->dev, pdev->fn, 0x10) & ~0xFu;
    if (!mmio) { pr_warn("xHCI MMIO BAR missing"); return -1; }

    uint32_t cmd = pci_read_config_dword(pdev->bus, pdev->dev, pdev->fn, 0x04);
    pci_write_config_dword(pdev->bus, pdev->dev, pdev->fn, 0x04, cmd | 0x06);

    // Try MSI-X first
    {
        volatile struct msix_table_entry *table = NULL;
        uint32_t table_size = 0;
        int cap_off = pci_msix_support(pdev);
        if (cap_off) {
            if (pci_msix_table_map(pdev, &table, &table_size) == 0 && table_size > 0) {
                int vec = msix_alloc_vector();
                if (vec > 0) {
                    msix_register_handler(vec, xhci_irq_handler);
                    pci_msix_enable(pdev, vec, table, 0);
                    pr_info("xHCI MSI-X enabled");
                    goto probe_done;
                }
            }
        }
        // Fallback: INTx via IOAPIC PCI vector
        extern int apic_pci_vector(uint8_t irq_pin);
        extern void set_idt_gate(int n, uint32_t handler);
        extern void xhci_isr();
        int vec = apic_pci_vector(pdev->irq_pin);
        if (vec > 0) {
            set_idt_gate(vec, (uint32_t)xhci_isr);
            pr_warn("xHCI MSI-X unavailable, using INTx");
        } else {
            pr_err("xHCI: no usable interrupt");
            return -1;
        }
    }

probe_done:
    cmd = pci_read_config_dword(pdev->bus, pdev->dev, pdev->fn, 0x04);
    pci_write_config_dword(pdev->bus, pdev->dev, pdev->fn, 0x04, cmd & ~(1u << 10));

    return xhci_init_one(mmio);
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

    int found = 0;
    pci_device_t *d = pci_find_by_class(0x0C, 0x03);
    while (d) {
        if (d->prog_if == 0x30) {
            found++;
            xhci_pci_probe(d);
            d->drv_probe_state = 2;
        }
        d = d->next;
        while (d && !(d->class_code == 0x0C && d->subclass == 0x03 && d->prog_if == 0x30))
            d = d->next;
    }
    if (found == 0) {
        pr_warn("xHCI: no USB3 controller found on PCI bus");
    } else {
        pr_info("xHCI host controller(s) brought up");
    }
}
