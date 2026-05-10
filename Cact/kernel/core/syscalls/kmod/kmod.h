#ifndef SC_KMOD_H
#define SC_KMOD_H

#include <stdint.h>

/*
 * Load a PCI relocatable module (path to ET_REL .o) for one vendor:device.
 * Ring 3: effective UID must be 0. Wildcard IDs (0xFFFF) are rejected.
 * Re-match is run for all enumerated PCI devices (first matching driver wins).
 */
int sys_module_load(const char* path, uint32_t vendor_id, uint32_t device_id);

/* Unload the usermod slot (PCI driver "usermod") — root only. */
int sys_module_unload(void);

#endif
