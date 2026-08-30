#include "pci_gdd.h"
#include "pci_driver.h"
#include "pci_loader.h"
#include "pci_modblob.h"
#include "fs_mod.h"
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

    printk("Device Found: ");
    printk((char *)ent->human_name);
    printk(" (");
    printk((char *)cat);
    printk("). Load driver ");
    printk((char *)ent->module_path);
    printk("? (y/n) [5s timeout]\n");

    if (!gdd_prompt_yes_no()) {
        printk("[GDD] skipped (timeout or 'n'): ");
        printk((char *)ent->human_name);
        printk("\n");
        return;
    }

    pci_driver_t *exist = pci_driver_find_class_module(ent->class_code, ent->subclass, ent->module_path);
    if (exist && exist->probe) {
        printk("[GDD] driver already resident for ");
        printk((char *)ent->human_name);
        printk("\n");
        return;
    }

    pci_driver_t *drv = (pci_driver_t *)kmalloc(sizeof(pci_driver_t));
    char         *path_copy;
    char          seqbuf[16];

    if (!drv) {
        printk("[GDD] kmalloc(drv) failed\n");
        return;
    }

    int plen = strlen((char *)ent->module_path) + 1;
    path_copy     = (char *)kmalloc((uint32_t)plen);
    if (!path_copy) {
        kfree(drv);
        printk("[GDD] kmalloc(path) failed\n");
        return;
    }
    memcpy(path_copy, ent->module_path, (unsigned int)plen);

    memset(drv, 0, sizeof *drv);
    int pos = buf_append(drv->name, 0, PCI_DRIVER_NAME_MAX, "gdd");
    snprintf(seqbuf, sizeof(seqbuf), "%d", (int)((int)gdd_drv_seq++));
    buf_append(drv->name, pos, PCI_DRIVER_NAME_MAX, seqbuf);

    drv->vendor_id   = ent->vendor_id;
    drv->device_id   = ent->device_id;
    drv->class_code  = ent->class_code;
    drv->subclass    = ent->subclass;
    drv->module_path = path_copy;
    drv->probe       = NULL;

    if (pci_register_driver(drv) != 0) {
        kfree(path_copy);
        kfree(drv);
        printk("[GDD] pci_register_driver failed\n");
        return;
    }

    printk("[GDD] lazy driver registered: ");
    printk((char *)ent->module_path);
    printk("\n");
}

void pci_gdd_prompt_devices(void) {
    extern pci_device_t *pci_device_list;
    for (pci_device_t *d = pci_device_list; d; d = d->next)
        pci_user_prompt_module(d->class_code, d->subclass, d->prog_if, d);
}

/* Human-readable name from a module path: "/lib/mdls/btrfs.cctk" -> "btrfs". */
static void fs_module_display_name(const char *path, char *out, int out_sz) {
    const char *base = path;
    for (const char *p = path; p && *p; p++)
        if (*p == '/') base = p + 1;

    int i = 0;
    for (; base[i] && base[i] != '.' && i < out_sz - 1; i++)
        out[i] = base[i];
    out[i] = '\0';
}

/* Non-PCI filesystem driver load. Unlike PCI devices a filesystem module is
 * not bound to a VID:DID/class — instead we enumerate the staged cctkfs
 * image and offer every module that exports the generic 'fs_mount' symbol.
 * This keeps GDD agnostic to *which* filesystem (ext4, btrfs, ...) is packed
 * in. If none are staged or all are declined, mntfs uses the virtual nodisk
 * root. Exactly one filesystem module may be loaded (fs_mod is single-slot). */
void pci_gdd_prompt_fs(void) {
    int count = pci_modblob_count();
    if (count <= 0)
        return;

    for (int i = 0; i < count; i++) {
        const char    *path = NULL;
        const uint8_t *data = NULL;
        uint32_t       size = 0;
        if (pci_modblob_at(i, &path, &data, &size) != 0 || !path)
            continue;

        if (fs_mod_detect(path) != 1)
            continue;

        char fname[32];
        fs_module_display_name(path, fname, sizeof(fname));

        printk("File System Found: ");
        printk(fname);
        printk(" (");
        printk((char *)path);
        printk("). Load driver? (y/n) [5s timeout]\n");

        if (!gdd_prompt_yes_no()) {
            printk("[GDD] filesystem driver skipped (timeout or 'n'): ");
            printk(fname);
            printk("\n");
            continue;
        }

        if (fs_mod_load(path) != 0) {
            printk("[GDD] filesystem driver load failed: ");
            printk(fname);
            printk("\n");
            continue;
        }
        printk("[GDD] filesystem driver loaded: ");
        printk(fname);
        printk("\n");
        break;   // single-slot; stop after the first accepted module
    }
}
