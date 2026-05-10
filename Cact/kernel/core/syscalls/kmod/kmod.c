#include "kmod.h"
#include "validate.h"
#include "helper.h"
#include "task.h"
#include "pci_driver.h"
#include "pci_loader.h"
#include "pci_enum.h"
#include "klib.h"
#include "kernel.h"

static pci_driver_t usermod_pci_drv;
static char         usermod_path_store[256];
static int          usermod_slot_active;

static int require_root(void) {
    if (!current_task)
        return -1;
    if (current_task->is_kernel)
        return 0;
    if (current_task->euid != 0)
        return -1;
    return 0;
}

int sys_module_load(const char* path, uint32_t vendor_id, uint32_t device_id) {
    if (require_root() != 0)
        return -1;
    if (!validate_user_str(path))
        return -2;

    if (usermod_slot_active) {
        kprint("[KMOD] usermod slot busy — call module_unload first\n");
        return -3;
    }

    _kstrcpy(usermod_path_store, path, (int)sizeof(usermod_path_store));

    uint16_t v, d;
    uint8_t  cc = (uint8_t)PCI_ANY_ID;
    uint8_t  ss = (uint8_t)PCI_ANY_ID;

    int auto_ids =
        (vendor_id == CACT_MODLOAD_ID_AUTO && device_id == CACT_MODLOAD_ID_AUTO);

    if (auto_ids) {
        if (pci_peek_module_manifest(usermod_path_store, &v, &d, &cc, &ss) != 0)
            return -2;
        kprint("[KMOD] usermod manifest: ");
        kprint_hex(v);
        kprint(":");
        kprint_hex(d);
        kprint(" (from module)\n");
    } else {
        if (vendor_id > 0xFFFFu || device_id > 0xFFFFu)
            return -2;
        if (vendor_id == PCI_ANY_ID || device_id == PCI_ANY_ID)
            return -2;
        v = (uint16_t)vendor_id;
        d = (uint16_t)device_id;
    }

    memset(&usermod_pci_drv, 0, sizeof(usermod_pci_drv));
    strncpy(usermod_pci_drv.name, "usermod", PCI_DRIVER_NAME_MAX - 1);
    usermod_pci_drv.name[PCI_DRIVER_NAME_MAX - 1] = '\0';
    usermod_pci_drv.vendor_id  = v;
    usermod_pci_drv.device_id  = d;
    usermod_pci_drv.class_code = cc;
    usermod_pci_drv.subclass   = ss;
    usermod_pci_drv.module_path = usermod_path_store;
    usermod_pci_drv.probe       = NULL;

    if (pci_register_driver(&usermod_pci_drv) != 0)
        return -2;

    for (pci_device_t* d = pci_device_list; d; d = d->next)
        pci_driver_match(d);

    if (!usermod_pci_drv.probe) {
        kprint("[KMOD] usermod: probe not linked — no PCI dev for this VID/DID, or "
               "[LDR]/[DRV] load error (check messages above)\n");
        pci_unregister_driver(&usermod_pci_drv);
        return -4;
    }

    usermod_slot_active = 1;
    kprint("[KMOD] usermod slot active\n");
    return 0;
}

int sys_module_unload(const char *name) {
    if (require_root() != 0)
        return -1;

    if (!name) {
        if (!usermod_slot_active)
            return 0;
        pci_unload_module(&usermod_pci_drv);
        pci_unregister_driver(&usermod_pci_drv);
        memset(&usermod_pci_drv, 0, sizeof(usermod_pci_drv));
        usermod_path_store[0] = '\0';
        usermod_slot_active   = 0;
        kprint("[KMOD] usermod unloaded\n");
        return 0;
    }

    if (!validate_user_str(name))
        return -2;

    pci_driver_t *drv = pci_driver_find_by_name(name);
    if (!drv) {
        kprint("[KMOD] no driver named: "); kprint((char *)name); kprint("\n");
        return -5;
    }
    if (!(drv->flags & PCI_DRV_F_RELOC_MODULE)) {
        kprint("[KMOD] not an ET_REL module (built-in): ");
        kprint((char *)name); kprint("\n");
        return -6;
    }

    pci_unload_module(drv);
    pci_unregister_driver(drv);

    if (drv == &usermod_pci_drv) {
        memset(&usermod_pci_drv, 0, sizeof(usermod_pci_drv));
        usermod_path_store[0] = '\0';
        usermod_slot_active   = 0;
    }

    kprint("[KMOD] unloaded: "); kprint((char *)name); kprint("\n");
    return 0;
}
