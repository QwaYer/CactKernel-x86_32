/*
 * cact_zstd.c — glue between the Cact kernel and the vendored zstd
 * decompressor.
 *
 * The decompressor is linked directly (not through Linux's exported zstd_*
 * wrappers in zstd_decompress_module.c), so the full static-linking API from
 * zstd_lib.h is available here.  A DCtx is placed in an aligned kmalloc'd
 * workspace for the duration of one call, mirroring Linux's decompress_single().
 */

#include <stdint.h>
#include <stddef.h>

#define ZSTD_STATIC_LINKING_ONLY
#include "compat/zstd.h"

#include "memory.h"
#include "cact_zstd.h"

size_t cact_zstd_frame_size(const void *src, size_t src_size)
{
	unsigned long long size;

	if (!src || !src_size)
		return 0;

	size = ZSTD_getFrameContentSize(src, src_size);
	if (size == ZSTD_CONTENTSIZE_UNKNOWN || size == ZSTD_CONTENTSIZE_ERROR)
		return 0;
	/* The callers index the result with uint32_t; a larger frame cannot be
	 * staged in the 32-bit kernel anyway. */
	if (size > 0xFFFFFFFFull)
		return 0;

	return (size_t)size;
}

size_t cact_zstd_decompress(const void *src, size_t src_size,
			    void *dst, size_t dst_capacity)
{
	size_t ws_size;
	void *ws;
	ZSTD_DCtx *dctx;
	size_t ret;

	if (!src || !src_size || !dst || !dst_capacity)
		return 0;

	ws_size = ZSTD_estimateDCtxSize();
	/* zstd requires the workspace 8-byte aligned; kmalloc() only guarantees
	 * the heap's block alignment, so ask for an explicitly aligned block. */
	ws = kmalloc_aligned((uint32_t)ws_size, 16);
	if (!ws)
		return 0;

	dctx = ZSTD_initStaticDCtx(ws, ws_size);
	if (!dctx) {
		kfree(ws);
		return 0;
	}

	ret = ZSTD_decompressDCtx(dctx, dst, dst_capacity, src, src_size);
	kfree(ws);

	if (ZSTD_isError(ret))
		return 0;

	return ret;
}
