#ifndef ZSTD_COMPRESS_ADVANCED_H
#define ZSTD_COMPRESS_ADVANCED_H

/*-*************************************
*  Dependencies
***************************************/

#include "../compat/zstd.h" /* ZSTD_CCtx */

/*-*************************************
*  Target Compressed Block Size
***************************************/

/* ZSTD_compressSuperBlock() :
 * Used to compress a super block when targetCBlockSize is being used.
 * The given block will be compressed into multiple sub blocks that are around targetCBlockSize. */
size_t ZSTD_compressSuperBlock(ZSTD_CCtx* zc,
                               void* dst, size_t dstCapacity,
                               void const* src, size_t srcSize,
                               unsigned lastBlock);

#endif /* ZSTD_COMPRESS_ADVANCED_H */
