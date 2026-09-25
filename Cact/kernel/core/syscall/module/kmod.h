#ifndef SC_KMOD_H
#define SC_KMOD_H

#include <stdint.h>

// Kernel-string variants used by the /dev/sys node-ioctl ABI.
int kmod_load_kpath(const char *path, uint32_t vendor_id, uint32_t device_id);
int kmod_unload_kname(const char *name);

// Listing for /proc/modules.
int         kmod_count(void);         // number of resident PCI modules
const char *kmod_name_at(int idx);    // idx-th resident module name, or NULL

#endif
