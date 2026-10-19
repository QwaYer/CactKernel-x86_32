#ifndef ZSTD_FAST_H
#define ZSTD_FAST_H

#include "../common/mem.h"      /* U32 */
#include "zstd_compress_internal.h"

void ZSTD_fillHashTable(ZSTD_MatchState_t* ms,
                        void const* end, ZSTD_dictTableLoadMethod_e dtlm,
                        ZSTD_tableFillPurpose_e tfp);
size_t ZSTD_compressBlock_fast(
        ZSTD_MatchState_t* ms, SeqStore_t* seqStore, U32 rep[ZSTD_REP_NUM],
        void const* src, size_t srcSize);
size_t ZSTD_compressBlock_fast_dictMatchState(
        ZSTD_MatchState_t* ms, SeqStore_t* seqStore, U32 rep[ZSTD_REP_NUM],
        void const* src, size_t srcSize);
size_t ZSTD_compressBlock_fast_extDict(
        ZSTD_MatchState_t* ms, SeqStore_t* seqStore, U32 rep[ZSTD_REP_NUM],
        void const* src, size_t srcSize);

#endif /* ZSTD_FAST_H */
