#ifndef FIRMWARE_H
#define FIRMWARE_H

#include <stdint.h>

/* Firmware blobs live under /lib/firmware/ inside the cctkfs archive that the
 * bootloader hands the kernel as a multifile module.  request_firmware() copies
 * the blob out of that archive so the caller owns a stable buffer: the archive
 * is shared with driver modules, and a blob can be looked up while another is
 * still in use.
 *
 * On success *data is a kernel-owned buffer of *len bytes that stays valid
 * until release_firmware() is called on it.  Returns 0 on success, negative if
 * the blob is not present. */
int  request_firmware(const char *name, const uint8_t **data, uint32_t *len);
void release_firmware(const uint8_t *data);

#endif
