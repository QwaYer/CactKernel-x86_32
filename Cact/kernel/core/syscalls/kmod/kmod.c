#include "kmod.h"
#include "validate.h"
#include "helper.h"
#include "task.h"
#include "pci_driver.h"
#include "pci_loader.h"
#include "pci_enum.h"
#include "klib.h"
#include "kernel.h"

static int user_str_all_decimal(const char *name) {
    const char *p;
    if (!name || !name[0]) return 0;
    for (p = name; *p; p++) {
        if (*p < '0' || *p > '9') return 0;
    }
    return 1;
}

static int parse_pci_modinfo_index(const char *name, int *out_idx) {
    int v = 0;
    const char *p;
    for (p = name; *p; p++) {
        int dig = *p - '0';
        if (v > (0x7fffffff - dig) / 10) return -1;
        v = v * 10 + dig;
    }
    if (v < 1) return -1;
    *out_idx = v;
    return 0;
}

static pci_driver_t usermod_pci_drv;
static char         usermod_path_store[256];
static int          usermod_slot_active;

static int require_root(void) {
    if (!current_task)
        return -1;
    if (current_task->is_kernel)
        return 0;
    if (current_task->proc->euid != 0)
        return -1;
    return 0;
}

int sys_module_load(const char* path, uint32_t vendor_id, uint32_t device_id) {
    if (require_root() != 0)
        return -1;
    if (!validate_user_str(path))
        return -2;

    if (usermod_slot_active) {
        pr_warn("kmod slot busy");
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
        pr_warn("kmod probe not linked");
        pci_unregister_driver(&usermod_pci_drv);
        return -4;
    }

    usermod_slot_active = 1;
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
        return 0;
    }

    if (!validate_user_str(name))
        return -2;

    if (user_str_all_decimal(name)) {
        int idx;
        if (parse_pci_modinfo_index(name, &idx) != 0) {
            pr_warn("kmod invalid pci index");
            return -2;
        }
        pci_device_t *dev = pci_device_by_index(idx);
        if (!dev) {
            pr_warn("kmod pci function index not found");
            return -7;
        }
        pci_driver_t *rdrv = pci_driver_find_reloc_for_device(dev);
        if (!rdrv) {
            pr_warn("kmod no relocatable module for pci function");
            return -8;
        }
        pci_unload_module(rdrv);
        pci_unregister_driver(rdrv);
        if (rdrv == &usermod_pci_drv) {
            memset(&usermod_pci_drv, 0, sizeof(usermod_pci_drv));
            usermod_path_store[0] = '\0';
            usermod_slot_active   = 0;
        }
        return 0;
    }

    pci_driver_t *drv = pci_driver_find_by_name(name);
    if (!drv) {
        pr_warn("kmod driver not found");
        return -5;
    }
    if (!(drv->flags & PCI_DRV_F_RELOC_MODULE)) {
        pr_warn("kmod driver is built-in");
        return -6;
    }

    pci_unload_module(drv);
    pci_unregister_driver(drv);

    if (drv == &usermod_pci_drv) {
        memset(&usermod_pci_drv, 0, sizeof(usermod_pci_drv));
        usermod_path_store[0] = '\0';
        usermod_slot_active   = 0;
    }

    return 0;
}
