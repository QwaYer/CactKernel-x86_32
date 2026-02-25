#include "slab.h"
#include "memory.h"  
#include "kernel.h"   
#include "sync.h"                           


static inline uint32_t align_up(uint32_t size, uint32_t align)
{
    return (size + align - 1) & ~(align - 1);
}

static uint32_t calc_objs_per_slab(uint32_t obj_size)
{
    uint32_t usable = PAGE_SIZE - sizeof(slab_t);
    uint32_t n = usable / obj_size;
    return (n < 1) ? 1 : n;
}


static slab_cache_t* g_cache_list = NULL;
static irq_spinlock_t g_cache_lock;


//  Предопределённые кэши: 8, 16, 32, 64, 128, 256, 512, 1024, 2048 байт   
#define GENERIC_CACHE_COUNT 9
static const uint32_t g_generic_sizes[GENERIC_CACHE_COUNT] = {
    8, 16, 32, 64, 128, 256, 512, 1024, 2048
};
static slab_cache_t* g_generic_caches[GENERIC_CACHE_COUNT];


static void list_push(slab_t** head, slab_t* s)
{
    s->prev = NULL;
    s->next = *head;
    if (*head) (*head)->prev = s;
    *head = s;
}

static void list_remove(slab_t** head, slab_t* s)
{
    if (s->prev) s->prev->next = s->next;
    else         *head = s->next;
    if (s->next) s->next->prev = s->prev;
    s->prev = s->next = NULL;
}


static slab_t* slab_create_slab(slab_cache_t* cache)
{
    void* page = kalloc();
    if (!page) return NULL;

    slab_t* s = (slab_t*)page;
    s->next     = NULL;
    s->prev     = NULL;
    s->inuse    = 0;
    s->capacity = cache->objs_per_slab;
    s->cache    = cache;

    uint8_t* obj_base = (uint8_t*)page + sizeof(slab_t);
    uint32_t obj_size = cache->obj_size;

    s->freelist = obj_base;
    for (uint32_t i = 0; i < s->capacity; i++) {
        void** slot = (void**)(obj_base + i * obj_size);
        if (i + 1 < s->capacity)
            *slot = obj_base + (i + 1) * obj_size;
        else
            *slot = NULL; 

        if (cache->ctor)
            cache->ctor((void*)slot);
    }

    cache->total_slabs++;
    return s;
}

static void slab_destroy_slab(slab_cache_t* cache, slab_t* s)
{
    if (cache->dtor) {
        uint8_t* obj_base = (uint8_t*)s + sizeof(slab_t);
        for (uint32_t i = 0; i < s->capacity; i++)
            cache->dtor(obj_base + i * cache->obj_size);
    }
    kfree_page(s);   
    cache->total_slabs--;
}


// PUBLIC API
slab_cache_t* slab_cache_create(const char* name,
                                uint32_t    obj_size,
                                void      (*ctor)(void*),
                                void      (*dtor)(void*))
{
    if (obj_size < SLAB_MIN_OBJ_SIZE) obj_size = SLAB_MIN_OBJ_SIZE;
    if (obj_size > SLAB_MAX_OBJ_SIZE) {
        kprint("[SLAB] obj_size too large (> 2048)\n");
        return NULL;
    }

    if (obj_size < sizeof(void*)) obj_size = sizeof(void*);

    obj_size = align_up(obj_size, 8);

    slab_cache_t* cache = (slab_cache_t*)kmalloc(sizeof(slab_cache_t));
    if (!cache) return NULL;

    uint32_t i;
    for (i = 0; i < SLAB_NAME_LEN - 1 && name[i]; i++)
        cache->name[i] = name[i];
    cache->name[i] = '\0';

    cache->obj_size      = obj_size;
    cache->objs_per_slab = calc_objs_per_slab(obj_size);
    cache->partial       = NULL;
    cache->full          = NULL;
    cache->free          = NULL;
    cache->total_slabs   = 0;
    cache->total_allocs  = 0;
    cache->total_frees   = 0;
    cache->ctor          = ctor;
    cache->dtor          = dtor;

    irq_spinlock_acquire(&g_cache_lock);
    cache->next  = g_cache_list;
    g_cache_list = cache;
    irq_spinlock_release(&g_cache_lock);

    return cache;
}


void* slab_alloc(slab_cache_t* cache)
{
    if (!cache) return NULL;

    irq_spinlock_acquire(&g_cache_lock);
    slab_t* s = cache->partial;

    if (!s) {
        s = cache->free;
        if (s) {
            list_remove(&cache->free, s);
            list_push(&cache->partial, s);
        }
    }

    if (!s) {
        irq_spinlock_release(&g_cache_lock);
        s = slab_create_slab(cache);
        if (!s) return NULL;
        irq_spinlock_acquire(&g_cache_lock);
        list_push(&cache->partial, s);
    }

    void* obj = s->freelist;
    s->freelist = *(void**)obj;  
    s->inuse++;
    cache->total_allocs++;

    if (s->inuse == s->capacity) {
        list_remove(&cache->partial, s);
        list_push(&cache->full, s);
    }

    irq_spinlock_release(&g_cache_lock);
    return obj;
}


void slab_free(slab_cache_t* cache, void* obj)
{
    if (!cache || !obj) return;

    irq_spinlock_acquire(&g_cache_lock);

    slab_t* s = (slab_t*)((uint32_t)obj & ~(PAGE_SIZE - 1));

    if (s->cache != cache) {
        kprint("[SLAB] slab_free: wrong cache!\n");
        irq_spinlock_release(&g_cache_lock);
        return;
    }

    *(void**)obj = s->freelist;
    s->freelist = obj;

    uint32_t was_full = (s->inuse == s->capacity);
    s->inuse--;
    cache->total_frees++;

    if (was_full) {
        /* full → partial */
        list_remove(&cache->full, s);
        list_push(&cache->partial, s);
    } else if (s->inuse == 0) {
        /* partial → free */
        list_remove(&cache->partial, s);
        list_push(&cache->free, s);
    }

    irq_spinlock_release(&g_cache_lock);
}


void slab_cache_shrink(slab_cache_t* cache)
{
    if (!cache) return;

    irq_spinlock_acquire(&g_cache_lock);
    slab_t* s = cache->free;
    while (s) {
        slab_t* next = s->next;
        slab_destroy_slab(cache, s);
        s = next;
    }
    cache->free = NULL;
    irq_spinlock_release(&g_cache_lock);
}


void slab_cache_destroy(slab_cache_t* cache)
{
    if (!cache) return;

    irq_spinlock_acquire(&g_cache_lock);

    for (slab_t* s = cache->full;    s; ) { slab_t* n = s->next; slab_destroy_slab(cache, s); s = n; }
    for (slab_t* s = cache->partial; s; ) { slab_t* n = s->next; slab_destroy_slab(cache, s); s = n; }
    for (slab_t* s = cache->free;    s; ) { slab_t* n = s->next; slab_destroy_slab(cache, s); s = n; }
    cache->full = cache->partial = cache->free = NULL;

    slab_cache_t** pp = &g_cache_list;
    while (*pp && *pp != cache) pp = &(*pp)->next;
    if (*pp) *pp = cache->next;

    irq_spinlock_release(&g_cache_lock);

    kfree_heap(cache);
}


void slab_print_stats(const slab_cache_t* cache)
{
    if (!cache) return;

    uint32_t partial_slabs = 0, full_slabs = 0, free_slabs = 0;
    uint32_t free_objs = 0;

    for (slab_t* s = cache->partial; s; s = s->next) {
        partial_slabs++;
        free_objs += (s->capacity - s->inuse);
    }
    for (slab_t* s = cache->full; s; s = s->next) full_slabs++;
    for (slab_t* s = cache->free; s; s = s->next) {
        free_slabs++;
        free_objs += cache->objs_per_slab;
    }

    kprint("[SLAB] cache=");
    kprint(cache->name);
    kprint(" obj_size=");
    char buf[16];
    uint32_t v = cache->obj_size; int pos = 14; buf[15] = '\0';
    do { buf[pos--] = '0' + (v % 10); v /= 10; } while (v);
    kprint(buf + pos + 1);
    kprint(" slabs(full/partial/free)=");
    v = full_slabs; pos = 14;
    do { buf[pos--] = '0' + (v % 10); v /= 10; } while (v);
    kprint(buf + pos + 1); kprint("/");
    v = partial_slabs; pos = 14;
    do { buf[pos--] = '0' + (v % 10); v /= 10; } while (v);
    kprint(buf + pos + 1); kprint("/");
    v = free_slabs; pos = 14;
    do { buf[pos--] = '0' + (v % 10); v /= 10; } while (v);
    kprint(buf + pos + 1);
    kprint(" free_objs=");
    v = free_objs; pos = 14;
    do { buf[pos--] = '0' + (v % 10); v /= 10; } while (v);
    kprint(buf + pos + 1);
    kprint(" allocs=");
    v = cache->total_allocs; pos = 14;
    do { buf[pos--] = '0' + (v % 10); v /= 10; } while (v);
    kprint(buf + pos + 1);
    kprint("\n");
}


void slab_init(void)
{
    irq_spinlock_init(&g_cache_lock);
    g_cache_list = NULL;

    for (uint32_t i = 0; i < GENERIC_CACHE_COUNT; i++) {
        char name[SLAB_NAME_LEN] = "kmalloc-";
        uint32_t sz = g_generic_sizes[i];
        char num[8]; int p = 6; num[7] = '\0';
        do { num[p--] = '0' + (sz % 10); sz /= 10; } while (sz);
        uint32_t j = 8;
        for (uint32_t k = p + 1; k < 7 && j < SLAB_NAME_LEN - 1; k++, j++)
            name[j] = num[k];
        name[j] = '\0';

        g_generic_caches[i] = slab_cache_create(name, g_generic_sizes[i],
                                                 NULL, NULL);
    }
}


void* slab_kmalloc(uint32_t size)
{
    if (size == 0) return NULL;
    for (uint32_t i = 0; i < GENERIC_CACHE_COUNT; i++) {
        if (size <= g_generic_sizes[i])
            return slab_alloc(g_generic_caches[i]);
    }
    return kmalloc(size);
}


void slab_kfree(void* ptr)
{
    if (!ptr) return;
    slab_t* s = (slab_t*)((uint32_t)ptr & ~(PAGE_SIZE - 1));

    if (s->cache && s->capacity > 0 && s->capacity <= 512) {
        slab_free(s->cache, ptr);
        return;
    }

    kfree_heap(ptr);
}