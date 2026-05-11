#ifndef PCI_GDD_CONFIG_H
#define PCI_GDD_CONFIG_H

/* Modules live in cctkfs loaded by GRUB. Keep this list sorted by priority:
 * 1) exact VID:DID matches, 2) class/subclass/prog_if fallbacks. */
static const pci_gdd_entry_t pci_gdd_table[] = {
    { 0x11AB, 0x4354, 0x02, 0x00, PCI_GDD_PI_ANY, "Marvell Yukon 88E8040", "/proc/bin/mdls/yukon.cctk" },
    { PCI_ANY_ID, PCI_ANY_ID, 0x01, 0x01, PCI_GDD_PI_ANY, "IDE Controller",        "/proc/bin/mdls/ide.cctk" },
    { PCI_ANY_ID, PCI_ANY_ID, 0x01, 0x06, 0x01,           "AHCI SATA",             "/proc/bin/mdls/ahci.cctk" },
    { PCI_ANY_ID, PCI_ANY_ID, 0x01, 0x08, PCI_GDD_PI_ANY, "NVMe Storage",          "/proc/bin/mdls/nvme.cctk" },
    { PCI_ANY_ID, PCI_ANY_ID, 0x02, 0x00, PCI_GDD_PI_ANY, "Ethernet (virtio-net)", "/proc/bin/mdls/virtio_net.cctk" },
    { PCI_ANY_ID, PCI_ANY_ID, 0x03, 0x00, PCI_GDD_PI_ANY, "VGA Compatible",        "/proc/bin/mdls/vga.cctk" },
    { 0x0000, 0x0000, 0x00, 0x00, 0x00, NULL, NULL },
};

#endif
