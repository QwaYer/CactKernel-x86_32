/* This file provides custom allocation primitives
 */

#define ZSTD_DEPS_NEED_MALLOC
#include "zstd_deps.h"   /* ZSTD_malloc, ZSTD_calloc, ZSTD_free, ZSTD_memset */

#include "compiler.h" /* MEM_STATIC */
#define ZSTD_STATIC_LINKING_ONLY
#include "../compat/zstd.h" /* ZSTD_customMem */

#ifndef ZSTD_ALLOCATIONS_H
#define ZSTD_ALLOCATIONS_H

/* custom memory allocation functions */

MEM_STATIC void* ZSTD_customMalloc(size_t size, ZSTD_customMem customMem)
{
    if (customMem.customAlloc)
        return customMem.customAlloc(customMem.opaque, size);
    return ZSTD_malloc(size);
}

MEM_STATIC void* ZSTD_customCalloc(size_t size, ZSTD_customMem customMem)
{
    if (customMem.customAlloc) {
        /* calloc implemented as malloc+memset;
         * not as efficient as calloc, but next best guess for custom malloc */
        void* const ptr = customMem.customAlloc(customMem.opaque, size);
        ZSTD_memset(ptr, 0, size);
        return ptr;
    }
    return ZSTD_calloc(1, size);
}

MEM_STATIC void ZSTD_customFree(void* ptr, ZSTD_customMem customMem)
{
    if (ptr!=NULL) {
        if (customMem.customFree)
            customMem.customFree(customMem.opaque, ptr);
        else
            ZSTD_free(ptr);
    }
}

#endif /* ZSTD_ALLOCATIONS_H */
