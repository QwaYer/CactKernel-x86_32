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
    uint16_t    vendor_id;
    uint16_t    device_id;
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

/* Driver matching config lives in a dedicated header so operator can tune
 * VID:DID/class bindings without touching GDD logic. */
#include "pci_gdd_config.h"

static const char *pci_gdd_category(uint8_t cl) {
    for (int i = 0; pci_class_names[i].category; i++) {
        if (pci_class_names[i].class_code == cl)
            return pci_class_names[i].category;
    }
    return "Unknown";
}

static const pci_gdd_entry_t *pci_gdd_lookup(const pci_device_t *dev) {
    uint8_t cl, sc, pi;
    const pci_gdd_entry_t *fallback = NULL;
    if (!dev) return NULL;
    cl = dev->class_code;
    sc = dev->subclass;
    pi = dev->prog_if;

    for (int i = 0; pci_gdd_table[i].human_name; i++) {
        const pci_gdd_entry_t *e = &pci_gdd_table[i];
        if (e->vendor_id == PCI_ANY_ID || e->device_id == PCI_ANY_ID)
            continue;
        if (e->vendor_id == dev->vendor_id && e->device_id == dev->device_id)
            return e;
    }

    for (int i = 0; pci_gdd_table[i].human_name; i++) {
        const pci_gdd_entry_t *e = &pci_gdd_table[i];
        if (e->vendor_id != PCI_ANY_ID || e->device_id != PCI_ANY_ID)
            continue;
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

    const pci_gdd_entry_t *ent = pci_gdd_lookup(dev);
    if (!ent)
        return;

    /* If module is absent, do not prompt user at all. */
    {
        uint16_t vtmp = 0, dtmp = 0;
        uint8_t  ctmp = (uint8_t)PCI_ANY_ID, stmp = (uint8_t)PCI_ANY_ID;
        if (pci_peek_module_manifest(ent->module_path, &vtmp, &dtmp, &ctmp, &stmp) != 0)
            return;
    }

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

    pci_driver_t *exist = pci_driver_find_class_module(ent->class_code, ent->subclass, ent->module_path);
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

    drv->vendor_id   = ent->vendor_id;
    drv->device_id   = ent->device_id;
    drv->class_code  = ent->class_code;
    drv->subclass    = ent->subclass;
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
