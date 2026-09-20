#include "kmod.h"
#include "task.h"
#include "pci_driver.h"
#include "pci_loader.h"
#include "pci_enum.h"
#include "fs_mod.h"
#include "klib.h"
#include "kernel.h"

// Linux i386 errno values.  The /dev/sys ioctl returns these negated and the
// userspace wrapper (CactLibc nio_map) folds a negative result into errno, so
// modload/modunload can report *why* a transfer failed.  Returning the raw
// -1..-8 codes the callers used to invent is not an option: every one of them
// collapses to -1 (EPERM) on the way out, which is why every failure used to
// print "permission denied (need root)".
#ifndef EPERM
#define EPERM  1
#endif
#ifndef ENOENT
#define ENOENT 2
#endif
#ifndef EACCES
#define EACCES 13
#endif
#ifndef EBUSY
#define EBUSY  16
#endif
#ifndef EEXIST
#define EEXIST 17
#endif
#ifndef ENODEV
#define ENODEV 19
#endif
#ifndef EINVAL
#define EINVAL 22
#endif
#ifndef EOPNOTSUPP
#define EOPNOTSUPP 95
#endif
#ifndef ENOSPC
#define ENOSPC 28
#endif

// Module load/unload core.  These are reached through the /dev/sys node
// ioctls (CACT_SYSCTL_MODULE_LOAD/UNLOAD); the old sys_module_* trap
// wrappers were removed.

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

// Resident PCI module slots.  A .cctk PCI module occupies one slot for as long
// as it stays loaded; the slot owns the pci_driver_t handed to
// pci_register_driver() plus the module path it has to keep alive (module_path
// is read back on lazy load and printed by /dev/modinfo).  Several modules can
// be resident at once — this replaces the single "usermod" slot, which made the
// second modload fail with EBUSY.
#define KMOD_MAX_SLOTS 8
#define KMOD_PATH_MAX  256

typedef struct {
    int          used;
    char         name[PCI_DRIVER_NAME_MAX];
    char         path[KMOD_PATH_MAX];
    pci_driver_t drv;
} kmod_slot_t;

static kmod_slot_t kmod_slots[KMOD_MAX_SLOTS];

// Instance name: module basename with the trailing ".cctk" stripped, spaces
// folded to underscores ("/lib/virtio_net.cctk" -> "virtio_net").  This is the
// name the module is registered and unloaded under.
static void kmod_instance_name(const char *path, char *out, int out_sz) {
    const char *base = path;
    for (const char *p = path; p && *p; p++)
        if (*p == '/') base = p + 1;

    int i = 0;
    while (base[i] && i < out_sz - 1) {
        char c = (base[i] == ' ') ? '_' : base[i];
        if (c == '.' && base[i + 1] == 'c' && base[i + 2] == 'c' &&
            base[i + 3] == 't' && base[i + 4] == 'k')
            break;
        out[i++] = c;
    }
    out[i] = '\0';
}

static kmod_slot_t *kmod_slot_free(void) {
    for (int i = 0; i < KMOD_MAX_SLOTS; i++)
        if (!kmod_slots[i].used)
            return &kmod_slots[i];
    return 0;
}

static kmod_slot_t *kmod_slot_by_name(const char *name) {
    if (!name || !name[0]) return 0;
    for (int i = 0; i < KMOD_MAX_SLOTS; i++)
        if (kmod_slots[i].used && streq(kmod_slots[i].name, name))
            return &kmod_slots[i];
    return 0;
}

static kmod_slot_t *kmod_slot_by_drv(const pci_driver_t *drv) {
    for (int i = 0; i < KMOD_MAX_SLOTS; i++)
        if (kmod_slots[i].used && &kmod_slots[i].drv == drv)
            return &kmod_slots[i];
    return 0;
}

// Tear a resident module down: run its remove(), free the image, drop the driver
// from the table and free the slot it came from.  Drivers that no slot owns
// (none exist today, but pci_driver_t.module_path allows lazy loading) are torn
// down the same way and simply leave the slot table alone.
static void kmod_release_driver(pci_driver_t *drv) {
    if (!drv) return;
    kmod_slot_t *slot = kmod_slot_by_drv(drv);
    pci_unload_module(drv);
    pci_unregister_driver(drv);
    if (slot)
        memset(slot, 0, sizeof(*slot));
}

static int require_root(void) {
    if (!current_task)
        return -1;
    if (current_task->is_kernel)
        return 0;
    if (current_task->proc->euid != 0)
        return -1;
    return 0;
}

// pci_peek_module_manifest() codes -> errno.
static int peek_errno(int pr) {
    if (pr == -1)
        return -ENOENT;   // file not found / unreadable
    if (pr == -5)
        return -EACCES;   // HMAC signature rejected
    return -EINVAL;       // not ET_REL, corrupted, or no cact_pci_* manifest
}

// Kernel-string core: load a module whose image path is a kernel buffer.
// The module takes a free slot under its instance name; loading the same
// instance twice is EEXIST and running out of slots is ENOSPC.
int kmod_load_kpath(const char *path, uint32_t vendor_id, uint32_t device_id) {
    if (require_root() != 0)
        return -EPERM;
    if (!path || !path[0])
        return -EINVAL;

    // Filesystem modules (export fs_mount instead of a PCI manifest) are
    // loaded through the multi-slot fs_mod loader. Detection is a cheap
    // non-destructive symbol scan; the real HMAC verification happens in
    // fs_mod_load().  fs_mod_* already returns -errno.
    if (fs_mod_detect(path) == 1)
        return fs_mod_load(path);

    char name[PCI_DRIVER_NAME_MAX];
    kmod_instance_name(path, name, (int)sizeof(name));
    if (!name[0])
        return -EINVAL;

    if (kmod_slot_by_name(name)) {
        pr_warn("kmod module already loaded: %s", name);
        return -EEXIST;
    }

    uint16_t v, d;
    uint8_t  cc = (uint8_t)PCI_ANY_ID;
    uint8_t  ss = (uint8_t)PCI_ANY_ID;

    int auto_ids =
        (vendor_id == CACT_MODLOAD_ID_AUTO && device_id == CACT_MODLOAD_ID_AUTO);

    if (auto_ids) {
        int pr = pci_peek_module_manifest(path, &v, &d, &cc, &ss);
        if (pr != 0)
            return peek_errno(pr);
    } else {
        if (vendor_id > 0xFFFFu || device_id > 0xFFFFu)
            return -EINVAL;
        if (vendor_id == PCI_ANY_ID || device_id == PCI_ANY_ID)
            return -EINVAL;
        v = (uint16_t)vendor_id;
        d = (uint16_t)device_id;
    }

    kmod_slot_t *slot = kmod_slot_free();
    if (!slot) {
        pr_warn("kmod no free module slot");
        return -ENOSPC;
    }

    strlcpy(slot->path, path, (int)sizeof(slot->path));
    strlcpy(slot->name, name, (int)sizeof(slot->name));
    memset(&slot->drv, 0, sizeof(slot->drv));
    strlcpy(slot->drv.name, name, (int)sizeof(slot->drv.name));
    slot->drv.vendor_id  = v;
    slot->drv.device_id  = d;
    slot->drv.class_code = cc;
    slot->drv.subclass   = ss;
    slot->drv.module_path = slot->path;
    slot->drv.probe       = NULL;
    slot->used = 1;

    if (pci_register_driver(&slot->drv) != 0) {
        memset(slot, 0, sizeof(*slot));
        return -EEXIST;   // duplicate driver name / driver table full
    }

    for (pci_device_t *dev = pci_device_list; dev; dev = dev->next)
        pci_driver_match(dev);

    if (!slot->drv.probe) {
        // Manifest parsed fine, but no enumerated PCI function matched its
        // VID/DID (or the image failed to relocate and the probe was never
        // linked).  Look for the "[LDR]" lines in the kernel log.
        pr_warn("kmod probe not linked: %s", name);
        kmod_release_driver(&slot->drv);
        return -ENODEV;
    }

    pr_info("kmod module loaded: %s (slot %d)", name, (int)(slot - kmod_slots));
    return 0;
}

// Kernel-string core: unload by instance name / pci index, or every resident
// PCI module at once.  |name| is a kernel buffer or NULL (NULL unloads all).
int kmod_unload_kname(const char *name) {
    if (require_root() != 0)
        return -EPERM;

    if (!name) {
        for (int i = 0; i < KMOD_MAX_SLOTS; i++)
            if (kmod_slots[i].used)
                kmod_release_driver(&kmod_slots[i].drv);
        return 0;
    }

    // Filesystem module instance names ("ext4", ...) unload through fs_mod.
    if (fs_mod_loaded(name))
        return fs_mod_unload(name);

    if (user_str_all_decimal(name)) {
        int idx;
        if (parse_pci_modinfo_index(name, &idx) != 0) {
            pr_warn("kmod invalid pci index");
            return -EINVAL;
        }
        pci_device_t *dev = pci_device_by_index(idx);
        if (!dev) {
            pr_warn("kmod pci function index not found");
            return -ENOENT;
        }
        pci_driver_t *rdrv = pci_driver_find_reloc_for_device(dev);
        if (!rdrv) {
            pr_warn("kmod no relocatable module for pci function");
            return -ENODEV;
        }
        kmod_release_driver(rdrv);
        return 0;
    }

    pci_driver_t *drv = pci_driver_find_by_name(name);
    if (!drv) {
        pr_warn("kmod driver not found");
        return -ENOENT;
    }
    if (!(drv->flags & PCI_DRV_F_RELOC_MODULE)) {
        pr_warn("kmod driver is built-in");
        return -EOPNOTSUPP;
    }

    kmod_release_driver(drv);
    return 0;
}
