#ifndef CACT_ZSTD_H
#define CACT_ZSTD_H

#include <stdint.h>
#include <stddef.h>

/*
 * In-kernel zstd decompressor (ported from Linux 7.3.0-rc4, lib/zstd + xxhash).
 *
 * The firmware loader stages compressed blobs (*.zst) out of the cctkfs image
 * and unpacks them here.  Only single-frame, whole-buffer decompression is
 * exposed: a firmware blob is small and read in one piece, so the streaming
 * API is not needed.
 */

/* Uncompressed size declared by the frame header, or 0 when it is absent or
 * the input is not a valid zstd frame. */
size_t cact_zstd_frame_size(const void *src, size_t src_size);

/* Decompress one zstd frame from src into dst.  Returns the number of bytes
 * written, or 0 on any failure (bad frame, truncation, dst too small). */
size_t cact_zstd_decompress(const void *src, size_t src_size,
                            void *dst, size_t dst_capacity);

#endif /* CACT_ZSTD_H */
