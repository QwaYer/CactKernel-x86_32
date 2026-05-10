#ifndef SC_KMOD_H
#define SC_KMOD_H

#include <stdint.h>

int sys_module_load(const char* path, uint32_t vendor_id, uint32_t device_id);

int sys_module_unload(const char *name);

#endif
