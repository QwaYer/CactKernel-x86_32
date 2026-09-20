#include "msi.h"
#include "pci.h"
#include "pcidev.h"
#include "kernel.h"
#include "apic.h"

/* MSI Message Control (capability ID 0x05, 16-bit at cap+2). */
#define MSI_CTRL_ENABLE     (1u << 0)
#define MSI_CTRL_MMC_MASK   0x7u          /* bits 3:1 Multiple Message Capable */
#define MSI_CTRL_MME_SHIFT  4             /* bits 6:4 Multiple Message Enable  */
#define MSI_CTRL_MME_MASK   0x7u
#define MSI_CTRL_64BIT      (1u << 7)     /* 64-bit message address capable   */
#define MSI_CTRL_PVM        (1u << 8)     /* per-vector masking capable       */

int pci_msi_support(pci_device_t *dev)
{
    if (!dev) return 0;

    msidev_ensure_enabled(dev);

    /* MSI structures are DWORD-aligned and live in 0x40..0xFF, so the plain
     * capability walk is enough — unlike MSI-X there is no MMIO table that a
     * broken list pointer could hide, hence no brute-force scan here. */
    return msidev_find_cap(dev, PCI_CAP_ID_MSI);
}

int pci_msi_enable(pci_device_t *dev, int vector)
{
    int cap = pci_msi_support(dev);
    if (!cap) return -1;
    if (vector < MSIDEV_VECTOR_BASE || vector >= MSIDEV_VECTOR_END) return -1;

    uint16_t ctrl = pcidev_cfg_read16(dev, (uint16_t)(cap + 2));
    if (ctrl & MSI_CTRL_ENABLE) return 0;              /* idempotent */

    int is64 = (ctrl & MSI_CTRL_64BIT) != 0;
    uint16_t data_reg = (uint16_t)(cap + (is64 ? 0x0C : 0x08));

    /* Exactly one message (MME=0).  MME <= MMC is always legal, and a caller
     * that reserved a single vector must not leave the device free to pick a
     * different message number — that vector would never be dispatched. */
    ctrl &= (uint16_t)~(MSI_CTRL_MME_MASK << MSI_CTRL_MME_SHIFT);

    /* Program the message while MSI is still disabled: address and data must
     * be valid before the enable bit arms delivery. */
    pcidev_cfg_write32(dev, (uint16_t)(cap + 4), apic_msi_address());
    if (is64)
        pcidev_cfg_write32(dev, (uint16_t)(cap + 8), 0);
    pcidev_cfg_write16(dev, data_reg, (uint16_t)vector);

    /* Per-vector masking, when implemented: 0 unmasks.  Written before the
     * enable bit for the same reason as address/data. */
    if (ctrl & MSI_CTRL_PVM)
        pcidev_cfg_write32(dev, (uint16_t)(cap + (is64 ? 0x10 : 0x0C)), 0);

    /* Legacy INTx off, then MSI on.  Same ordering rationale as MSI-X: the
     * function must not be able to signal through a half-configured path. */
    uint32_t cmd = pcidev_cfg_read32(dev, 0x04);
    pcidev_cfg_write32(dev, 0x04, cmd | (1u << 10));

    pcidev_cfg_write16(dev, (uint16_t)(cap + 2),
                       (uint16_t)(ctrl | MSI_CTRL_ENABLE));
    __asm__ volatile("sfence" ::: "memory");

    pr_info("  %-11s : vec 0x%x enabled (cap 0x%x%s, %02x:%02x.%u)\n",
            "msi", (unsigned)vector, (unsigned)cap, is64 ? ", 64-bit" : "",
            (unsigned)dev->bus, (unsigned)dev->dev, (unsigned)dev->fn);
    return 0;
}
