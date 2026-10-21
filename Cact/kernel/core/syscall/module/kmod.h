#ifndef SC_KMOD_H
#define SC_KMOD_H

#include <stdint.h>

// Kernel-string variants used by the /dev/sys node-ioctl ABI.
int kmod_load_kpath(const char *path, uint32_t vendor_id, uint32_t device_id);
int kmod_unload_kname(const char *name);

// Listing for /proc/modules.  Slots are scanned directly (they can be sparse:
// unloading the first of two modules frees slot 0 while slot 1 stays used).
#define KMOD_MAX_SLOTS 8
int         kmod_count(void);         // number of resident PCI modules
const char *kmod_name_at(int slot);   // module name in slot, or NULL
uint32_t    kmod_size_at(int slot);   // resident image bytes, 0 if free

#endif
