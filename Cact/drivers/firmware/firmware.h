#ifndef FIRMWARE_H
#define FIRMWARE_H

#include <stdint.h>

/* Firmware blobs live under /usr/lib/firmware/ (or /usr/lib/).  They are looked
 * up in two places, in order:
 *   1. the cctkfs archive the bootloader hands the kernel as a multifile module;
 *   2. the VFS — a blob extracted there by firmextr (see CactUserBins) or
 *      installed by cactpkg.
 * A blob may be stored zstd-compressed (<name>.zst, as shipped by the firmware
 * mirror) and is unpacked on demand with the in-kernel zstd decompressor
 * (Cact/drivers/zstd); an uncompressed <name> is also accepted.
 *
 * request_firmware() copies (and, for a .zst blob, decompresses) the blob so the
 * caller owns a stable buffer: the cctkfs archive is shared with driver modules,
 * and a blob can be looked up while another is still in use.
 *
 * On success *data is a kernel-owned buffer of *len bytes that stays valid
 * until release_firmware() is called on it.  Returns 0 on success, negative if
 * the blob is not present. */
int  request_firmware(const char *name, const uint8_t **data, uint32_t *len);
void release_firmware(const uint8_t *data);

#endif
