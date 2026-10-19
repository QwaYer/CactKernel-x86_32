#ifndef INITFS_MODBLOB_H
#define INITFS_MODBLOB_H

#include <stdint.h>

/* Maximum supported size of the cctkfs image staged in .bss.
 * Packed userland (bin/sbin/lib + drivers + the cactpkg repository + the
 * installer media) is over 12 MiB.  This buffer dominates the kernel image's
 * bss, so raising it moves the image end up with it: keep the result below the
 * PMM reservation floor (HEAP_START/RESERVED_END in memory.h and ffi.rs), or
 * the tail of the image lands inside the heap window. */
#define INITFS_MODBLOB_MAX_IMAGE  (4096u * 6144u)   /* 24 MiB */

/* Initialise the in-memory module table from a cctkfs image previously
 * loaded by GRUB as a multiboot2 module.  Must be called BEFORE the
 * kernel heap touches anything below the module's physical address —
 * the data is copied into a static .bss buffer for safety.
 * Returns 0 on success, negative on bad/missing image. */
int initfs_modblob_load(uint32_t phys_addr, uint32_t size);

/* Lookup a module image by canonical path (e.g. "/usr/lib/modules/ahci.cctk").
 * On hit, *out_data points into the staged buffer and *out_size is the
 * payload size.  Returns 0 on hit, -1 on miss. */
int initfs_modblob_get(const char *path, const uint8_t **out_data,
                       uint32_t *out_size);

int initfs_modblob_count(void);
int initfs_modblob_at(int idx, const char **out_path,
                      const uint8_t **out_data, uint32_t *out_size);

#endif
