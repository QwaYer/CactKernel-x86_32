#ifndef SLAB_H
#define SLAB_H

#include <stdint.h>
#include <stddef.h>

#define SLAB_MAX_OBJ_SIZE  2048
#define SLAB_MIN_OBJ_SIZE  8
#define SLAB_NAME_LEN      32

typedef struct slab {
    struct slab*  next;
    struct slab*  prev;
    void*         freelist;
    uint32_t      inuse;
    uint32_t      capacity;
    struct slab_cache* cache;
} slab_t;

typedef struct slab_cache {
    char     name[SLAB_NAME_LEN];
    uint32_t obj_size;
    uint32_t objs_per_slab;
    slab_t*  partial;
    slab_t*  full;
    slab_t*  free;
    uint32_t total_slabs;
    uint32_t total_allocs;
    uint32_t total_frees;
    void (*ctor)(void* obj);
    void (*dtor)(void* obj);
    struct slab_cache* next;
} slab_cache_t;

slab_cache_t* slab_cache_create(const char* name, uint32_t obj_size,
                                void (*ctor)(void*), void (*dtor)(void*));
void*         slab_alloc(slab_cache_t* cache);
void          slab_free(slab_cache_t* cache, void* obj);
void          slab_cache_destroy(slab_cache_t* cache);
void          slab_cache_shrink(slab_cache_t* cache);
void          slab_print_stats(const slab_cache_t* cache);
void          slab_init(void);
void*         slab_kmalloc(uint32_t size);
void          slab_kfree(void* ptr);

#endif
