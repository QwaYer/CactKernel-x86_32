#include "pci_driver.h"
#include "pci_loader.h"
#include "pcidev.h"
#include "kernel.h"
#include "klib.h"

#define PCI_MODINFO_MAX 32768

int pci_driver_modinfo_read(uint32_t off, uint32_t size, char *buf)
{
    extern pci_driver_t *pcidev_driver_list;
    extern uint32_t      pcidev_driver_count;

    static char tmp[PCI_MODINFO_MAX];

    uint32_t nreg = pcidev_driver_count;

    int p = 0;
    p = buf_append(tmp, p, PCI_MODINFO_MAX, "# /dev/modinfo - PCI overview\n");
    p = buf_append(tmp, p, PCI_MODINFO_MAX, "# A: structs from pci_register_driver()  ");
    p = buf_append(tmp, p, PCI_MODINFO_MAX, "B: every function found by pci_enumerate()\n\n");

    p = buf_append(tmp, p, PCI_MODINFO_MAX, "# --- A. pci_register_driver() ---\n");
    p = buf_append(tmp, p, PCI_MODINFO_MAX, "# Drivers registered: ");
    p = buf_append_int(tmp, p, PCI_MODINFO_MAX, (int)nreg);
    p = buf_append(tmp, p, PCI_MODINFO_MAX, "  (0xFFFF = PCI_ANY_ID)\n");
    p = buf_append(tmp, p, PCI_MODINFO_MAX, "# \"probe bound\" = entry point ready ");
    p = buf_append(tmp, p, PCI_MODINFO_MAX, "(built-in or module loaded)\n\n");

    int idx = 0;
    char hx[16];
    for (pci_driver_t *d = pcidev_driver_list; d; d = d->next) {
        idx++;
        if (p >= PCI_MODINFO_MAX - 520) {
            p = buf_append(tmp, p, PCI_MODINFO_MAX, "\n# ... A truncated ...\n");
            break;
        }

        p = buf_append(tmp, p, PCI_MODINFO_MAX, "[drv ");
        p = buf_append_int(tmp, p, PCI_MODINFO_MAX, idx);
        p = buf_append(tmp, p, PCI_MODINFO_MAX, "] ");
        p = buf_append(tmp, p, PCI_MODINFO_MAX, d->name);
        p = buf_append(tmp, p, PCI_MODINFO_MAX, "\n");

        p = buf_append(tmp, p, PCI_MODINFO_MAX, "    vendor_id:       ");
        hex_to_ascii((unsigned int)d->vendor_id, hx);
        p = buf_append(tmp, p, PCI_MODINFO_MAX, hx);
        p = buf_append(tmp, p, PCI_MODINFO_MAX, "\n");

        p = buf_append(tmp, p, PCI_MODINFO_MAX, "    device_id:       ");
        hex_to_ascii((unsigned int)d->device_id, hx);
        p = buf_append(tmp, p, PCI_MODINFO_MAX, hx);
        p = buf_append(tmp, p, PCI_MODINFO_MAX, "\n");

        p = buf_append(tmp, p, PCI_MODINFO_MAX, "    class_code:      ");
        hex_to_ascii((unsigned int)d->class_code, hx);
        p = buf_append(tmp, p, PCI_MODINFO_MAX, hx);
        p = buf_append(tmp, p, PCI_MODINFO_MAX, "\n");

        p = buf_append(tmp, p, PCI_MODINFO_MAX, "    subclass:        ");
        hex_to_ascii((unsigned int)d->subclass, hx);
        p = buf_append(tmp, p, PCI_MODINFO_MAX, hx);
        p = buf_append(tmp, p, PCI_MODINFO_MAX, "\n");

        p = buf_append(tmp, p, PCI_MODINFO_MAX, "    module_path:     ");
        if (d->module_path)
            p = buf_append(tmp, p, PCI_MODINFO_MAX, (char *)d->module_path);
        else
            p = buf_append(tmp, p, PCI_MODINFO_MAX, "(none - built-in or no lazy path)");
        p = buf_append(tmp, p, PCI_MODINFO_MAX, "\n");

        p = buf_append(tmp, p, PCI_MODINFO_MAX, "    probe bound:     ");
        if (d->probe)
            p = buf_append(tmp, p, PCI_MODINFO_MAX, "yes\n");
        else
            p = buf_append(tmp, p, PCI_MODINFO_MAX, "no (lazy load not done yet)\n");

        p = buf_append(tmp, p, PCI_MODINFO_MAX, "    kmod unload:     ");
        if (d->flags & PCI_DRV_F_RELOC_MODULE)
            p = buf_append(tmp, p, PCI_MODINFO_MAX, "yes (sys_module_unload \"");
        else
            p = buf_append(tmp, p, PCI_MODINFO_MAX, "no (built-in, not ET_REL)\n");
        if (d->flags & PCI_DRV_F_RELOC_MODULE) {
            p = buf_append(tmp, p, PCI_MODINFO_MAX, d->name);
            p = buf_append(tmp, p, PCI_MODINFO_MAX, "\")\n");
        }

        p = buf_append(tmp, p, PCI_MODINFO_MAX, "\n");
    }

    p = buf_append(tmp, p, PCI_MODINFO_MAX, "# --- B. pci_enumerate() - PCI functions ---\n");
    p = buf_append(tmp, p, PCI_MODINFO_MAX, "# Devices in list: ");
    p = buf_append_int(tmp, p, PCI_MODINFO_MAX, (int)pci_device_count);
    p = buf_append(tmp, p, PCI_MODINFO_MAX, "\n\n");

    int bidx = 0;
    for (pci_device_t *dv = pci_device_list; dv; dv = dv->next) {
        bidx++;
        if (p >= PCI_MODINFO_MAX - 400) {
            p = buf_append(tmp, p, PCI_MODINFO_MAX, "\n# ... B truncated ...\n");
            break;
        }

        p = buf_append(tmp, p, PCI_MODINFO_MAX, "[pci ");
        p = buf_append_int(tmp, p, PCI_MODINFO_MAX, bidx);
        p = buf_append(tmp, p, PCI_MODINFO_MAX, "] ");
        p = buf_append(tmp, p, PCI_MODINFO_MAX, "bus ");
        p = buf_append_int(tmp, p, PCI_MODINFO_MAX, (int)dv->bus);
        p = buf_append(tmp, p, PCI_MODINFO_MAX, " dev ");
        p = buf_append_int(tmp, p, PCI_MODINFO_MAX, (int)dv->dev);
        p = buf_append(tmp, p, PCI_MODINFO_MAX, " fn ");
        p = buf_append_int(tmp, p, PCI_MODINFO_MAX, (int)dv->fn);
        p = buf_append(tmp, p, PCI_MODINFO_MAX, "\n");

        p = buf_append(tmp, p, PCI_MODINFO_MAX, "    vendor_id:       ");
        hex_to_ascii((unsigned int)dv->vendor_id, hx);
        p = buf_append(tmp, p, PCI_MODINFO_MAX, hx);
        p = buf_append(tmp, p, PCI_MODINFO_MAX, "\n");

        p = buf_append(tmp, p, PCI_MODINFO_MAX, "    device_id:       ");
        hex_to_ascii((unsigned int)dv->device_id, hx);
        p = buf_append(tmp, p, PCI_MODINFO_MAX, hx);
        p = buf_append(tmp, p, PCI_MODINFO_MAX, "\n");

        p = buf_append(tmp, p, PCI_MODINFO_MAX, "    class_code:      ");
        hex_to_ascii((unsigned int)dv->class_code, hx);
        p = buf_append(tmp, p, PCI_MODINFO_MAX, hx);
        p = buf_append(tmp, p, PCI_MODINFO_MAX, "\n");

        p = buf_append(tmp, p, PCI_MODINFO_MAX, "    subclass:        ");
        hex_to_ascii((unsigned int)dv->subclass, hx);
        p = buf_append(tmp, p, PCI_MODINFO_MAX, hx);
        p = buf_append(tmp, p, PCI_MODINFO_MAX, "\n");

        p = buf_append(tmp, p, PCI_MODINFO_MAX, "    prog_if:         ");
        hex_to_ascii((unsigned int)dv->prog_if, hx);
        p = buf_append(tmp, p, PCI_MODINFO_MAX, hx);
        p = buf_append(tmp, p, PCI_MODINFO_MAX, "\n");

        p = buf_append(tmp, p, PCI_MODINFO_MAX, "    revision:        ");
        hex_to_ascii((unsigned int)dv->revision, hx);
        p = buf_append(tmp, p, PCI_MODINFO_MAX, hx);
        p = buf_append(tmp, p, PCI_MODINFO_MAX, "\n");

        p = buf_append(tmp, p, PCI_MODINFO_MAX, "    header_type:     ");
        hex_to_ascii((unsigned int)dv->header_type, hx);
        p = buf_append(tmp, p, PCI_MODINFO_MAX, hx);
        p = buf_append(tmp, p, PCI_MODINFO_MAX, "\n");

        p = buf_append(tmp, p, PCI_MODINFO_MAX, "    irq_line:        ");
        p = buf_append_int(tmp, p, PCI_MODINFO_MAX, (int)dv->irq_line);
        p = buf_append(tmp, p, PCI_MODINFO_MAX, "\n");

        p = buf_append(tmp, p, PCI_MODINFO_MAX, "    irq_pin:         ");
        p = buf_append_int(tmp, p, PCI_MODINFO_MAX, (int)dv->irq_pin);
        p = buf_append(tmp, p, PCI_MODINFO_MAX, "\n\n");
    }

    int total = 0;
    while (tmp[total] && total < PCI_MODINFO_MAX - 1) total++;

    if (off >= (uint32_t)total) return 0;
    uint32_t avail = (uint32_t)total - off;
    if (size > avail) size = avail;
    memcpy(buf, tmp + off, size);
    return (int)size;
}
