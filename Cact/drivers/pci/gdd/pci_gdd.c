#include "pci_gdd.h"
#include "pci_driver.h"
#include "pci_loader.h"
#include "keyboard.h"
#include "kernel.h"
#include "klib.h"
#include "memory.h"
#include "helper.h"

#define GDD_PROMPT_TIMEOUT_TICKS  (5u * 100u)

typedef struct {
    uint8_t     class_code;
    const char *category;
} pci_class_name_t;

typedef struct {
    uint8_t     class_code;
    uint8_t     subclass;
    uint8_t     prog_if;
    const char *human_name;
    const char *module_path;
} pci_gdd_entry_t;

static const pci_class_name_t pci_class_names[] = {
    { 0x01, "Storage" },
    { 0x02, "Network" },
    { 0x03, "Video" },
    { 0x00, NULL },
};

/* Modules live in the cctkfs image GRUB loads alongside the kernel; the
 * user-visible mountpoint is /proc/bin/mdls/.  pci_modblob_get() accepts
 * either the canonical "/lib/<name>.cctk" or the mdls path, so this table
 * uses the path the operator actually sees in the prompt and on disk. */
static const pci_gdd_entry_t pci_gdd_table[] = {
    { 0x01, 0x01, PCI_GDD_PI_ANY, "IDE Controller",        "/proc/bin/mdls/ide.cctk" },
    { 0x01, 0x06, 0x01,           "AHCI SATA",             "/proc/bin/mdls/ahci.cctk" },
    { 0x01, 0x08, PCI_GDD_PI_ANY, "NVMe Storage",          "/proc/bin/mdls/nvme.cctk" },
    { 0x02, 0x00, PCI_GDD_PI_ANY, "Ethernet (virtio-net)", "/proc/bin/mdls/virtio_net.cctk" },
    { 0x03, 0x00, PCI_GDD_PI_ANY, "VGA Compatible",        "/proc/bin/mdls/vga.cctk" },
    { 0x00, 0x00, 0x00,           NULL,                    NULL },
};

static const char *pci_gdd_category(uint8_t cl) {
    for (int i = 0; pci_class_names[i].category; i++) {
        if (pci_class_names[i].class_code == cl)
            return pci_class_names[i].category;
    }
    return "Unknown";
}

static const pci_gdd_entry_t *pci_gdd_lookup(uint8_t cl, uint8_t sc, uint8_t pi) {
    const pci_gdd_entry_t *fallback = NULL;
    for (int i = 0; pci_gdd_table[i].human_name; i++) {
        const pci_gdd_entry_t *e = &pci_gdd_table[i];
        if (e->class_code != cl || e->subclass != sc)
            continue;
        if (e->prog_if == pi)
            return e;
        if (e->prog_if == PCI_GDD_PI_ANY)
            fallback = e;
    }
    return fallback;
}

/* Wait for y/n with timeout (timer_ticks @ 100 Hz). Temporarily enables IRQs. */
static int gdd_prompt_yes_no(void) {
    uint32_t ef;
    __asm__ volatile ("pushf; pop %0" : "=r"(ef));

    __asm__ volatile ("sti");

    uint32_t start = timer_ticks_get();

    for (;;) {
        int k = keyboard_read_char();
        if (k >= 0) {
            char c = (char)k;
            int  yes = (c == 'y' || c == 'Y');
            int  no  = (c == 'n' || c == 'N');
            if (yes || no) {
                if (!(ef & 0x200u))
                    __asm__ volatile ("cli");
                return yes ? 1 : 0;
            }
        }

        if ((timer_ticks_get() - start) >= GDD_PROMPT_TIMEOUT_TICKS) {
            if (!(ef & 0x200u))
                __asm__ volatile ("cli");
            return 0;
        }

        __asm__ volatile ("hlt");
    }
}

static uint32_t gdd_drv_seq;

void pci_user_prompt_module(uint8_t cl, uint8_t sc, uint8_t pi, pci_device_t *dev) {
    if (!dev)
        return;
    if (dev->class_code != cl || dev->subclass != sc || dev->prog_if != pi)
        return;

    const pci_gdd_entry_t *ent = pci_gdd_lookup(cl, sc, pi);
    if (!ent)
        return;

    const char *cat = pci_gdd_category(cl);

    kprint("Device Found: ");
    kprint((char *)ent->human_name);
    kprint(" (");
    kprint((char *)cat);
    kprint("). Load driver ");
    kprint((char *)ent->module_path);
    kprint("? (y/n) [5s timeout]\n");

    if (!gdd_prompt_yes_no()) {
        kprint("[GDD] skipped (timeout or 'n'): ");
        kprint((char *)ent->human_name);
        kprint("\n");
        return;
    }

    pci_driver_t *exist = pci_driver_find_class_module(cl, sc, ent->module_path);
    if (exist && exist->probe) {
        kprint("[GDD] driver already resident for ");
        kprint((char *)ent->human_name);
        kprint("\n");
        return;
    }

    pci_driver_t *drv = (pci_driver_t *)kmalloc(sizeof(pci_driver_t));
    char         *path_copy;
    char          seqbuf[16];

    if (!drv) {
        kprint("[GDD] kmalloc(drv) failed\n");
        return;
    }

    int plen = strlen((char *)ent->module_path) + 1;
    path_copy     = (char *)kmalloc((uint32_t)plen);
    if (!path_copy) {
        kfree_heap(drv);
        kprint("[GDD] kmalloc(path) failed\n");
        return;
    }
    memcpy(path_copy, ent->module_path, (unsigned int)plen);

    memset(drv, 0, sizeof *drv);
    strcpy(drv->name, "gdd");
    itoa((int)gdd_drv_seq++, seqbuf);
    strcat(drv->name, seqbuf);

    drv->vendor_id   = PCI_ANY_ID;
    drv->device_id   = PCI_ANY_ID;
    drv->class_code  = cl;
    drv->subclass    = sc;
    drv->module_path = path_copy;
    drv->probe       = NULL;

    if (pci_register_driver(drv) != 0) {
        kfree_heap(path_copy);
        kfree_heap(drv);
        kprint("[GDD] pci_register_driver failed\n");
        return;
    }

    kprint("[GDD] registered lazy driver for ");
    kprint((char *)ent->module_path);
    kprint(" — pci_driver_match follows\n");
}
