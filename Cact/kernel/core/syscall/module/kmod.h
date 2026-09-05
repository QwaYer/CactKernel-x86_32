#ifndef SC_KMOD_H
#define SC_KMOD_H

#include <stdint.h>

// Kernel-string variants used by the /dev/sys node-ioctl ABI.
int kmod_load_kpath(const char *path, uint32_t vendor_id, uint32_t device_id);
int kmod_unload_kname(const char *name);

#endif
