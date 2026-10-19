#ifndef ZSTD_COMPRESS_LITERALS_H
#define ZSTD_COMPRESS_LITERALS_H

#include "zstd_compress_internal.h" /* ZSTD_hufCTables_t, ZSTD_minGain() */


size_t ZSTD_noCompressLiterals (void* dst, size_t dstCapacity, const void* src, size_t srcSize);

/* ZSTD_compressRleLiteralsBlock() :
 * Conditions :
 * - All bytes in @src are identical
 * - dstCapacity >= 4 */
size_t ZSTD_compressRleLiteralsBlock (void* dst, size_t dstCapacity, const void* src, size_t srcSize);

/* ZSTD_compressLiterals():
 * @entropyWorkspace: must be aligned on 4-bytes boundaries
 * @entropyWorkspaceSize : must be >= HUF_WORKSPACE_SIZE
 * @suspectUncompressible: sampling checks, to potentially skip huffman coding
 */
size_t ZSTD_compressLiterals (void* dst, size_t dstCapacity,
                        const void* src, size_t srcSize,
                              void* entropyWorkspace, size_t entropyWorkspaceSize,
                        const ZSTD_hufCTables_t* prevHuf,
                              ZSTD_hufCTables_t* nextHuf,
                              ZSTD_strategy strategy, int disableLiteralCompression,
                              int suspectUncompressible,
                              int bmi2);

#endif /* ZSTD_COMPRESS_LITERALS_H */
