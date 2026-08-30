#ifndef INITFS_MODBLOB_H
#define INITFS_MODBLOB_H

#include <stdint.h>

/* Maximum supported size of the cctkfs image staged in .bss.
 * Packed userland (bin/sbin/lib + drivers) can exceed 256 KiB. */
#define INITFS_MODBLOB_MAX_IMAGE  (4096u * 4096u)

/* Initialise the in-memory module table from a cctkfs image previously
 * loaded by GRUB as a multiboot2 module.  Must be called BEFORE the
 * kernel heap touches anything below the module's physical address —
 * the data is copied into a static .bss buffer for safety.
 * Returns 0 on success, negative on bad/missing image. */
int initfs_modblob_load(uint32_t phys_addr, uint32_t size);

/* Lookup a module image by canonical path (e.g. "/lib/ahci.cctk").
 * On hit, *out_data points into the staged buffer and *out_size is the
 * payload size.  Returns 0 on hit, -1 on miss. */
int initfs_modblob_get(const char *path, const uint8_t **out_data,
                       uint32_t *out_size);

int initfs_modblob_count(void);
int initfs_modblob_at(int idx, const char **out_path,
                      const uint8_t **out_data, uint32_t *out_size);

#endif
