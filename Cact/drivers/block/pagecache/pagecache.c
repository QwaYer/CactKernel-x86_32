#include "pagecache.h"
#include "blkdev.h"
#include "kernel.h"
#include "memory.h"
#include "klib.h"

// Explicit redeclare in case transitive includes drop the prototypes
extern void* memory_set(void* dest, int val, int len);
extern void* memory_copy(void* dest, const void* src, int len);

// Internal page descriptor
struct page {
    uint32_t  dev;           // blkdev device id
    uint8_t   flags;         // PC_FLAG_*
    uint32_t  block_no;      // logical block number
    uint32_t  block_size;    // 512..PC_MAX_BLOCK_SIZE

    int       pin_count;     // >0 → must not be evicted

    struct page *hash_next;  // hash bucket chain

    struct page *lru_prev;   // LRU doubly-linked list
    struct page *lru_next;

    uint8_t  *data;          // dynamically allocated block buffer
};

// Static pool — no dynamic allocation after init
static struct page   pool[PC_MAX_PAGES];
static struct page  *hash_table[PC_HASH_SIZE];

// LRU sentinels
static struct page  *lru_head;
static struct page  *lru_tail;

// Statistics counters
static uint32_t stat_hits;
static uint32_t stat_misses;
static uint32_t stat_evictions;
static uint32_t stat_writebacks;

// Jenkins-style 32-bit hash for (dev, block_no)
static inline uint32_t _hash(uint32_t dev, uint32_t block_no) {
    uint32_t h = dev;
    h ^= block_no;
    h ^= h >> 16;
    h *= 0x45d9f3bU;
    h ^= h >> 16;
    return h & (PC_HASH_SIZE - 1);
}

// Remove page from hash table (caller must hold lock)
static void _hash_remove(struct page *p) {
    uint32_t bucket = _hash(p->dev, p->block_no);
    struct page **pp = &hash_table[bucket];
    while (*pp) {
        if (*pp == p) { *pp = p->hash_next; p->hash_next = 0; return; }
        pp = &(*pp)->hash_next;
    }
}

// Insert page into hash table (caller must hold lock)
static void _hash_insert(struct page *p) {
    uint32_t bucket = _hash(p->dev, p->block_no);
    p->hash_next         = hash_table[bucket];
    hash_table[bucket]   = p;
}

// Look up a page by (dev, block_no); returns NULL if not cached
static struct page *_hash_find(uint32_t dev, uint32_t block_no) {
    uint32_t bucket = _hash(dev, block_no);
    struct page *p  = hash_table[bucket];
    while (p) {
        if (p->dev == dev && p->block_no == block_no)
            return p;
        p = p->hash_next;
    }
    return 0;
}

// Unlink page from LRU list
static void _lru_remove(struct page *p) {
    if (p->lru_prev) p->lru_prev->lru_next = p->lru_next;
    else             lru_head              = p->lru_next;
    if (p->lru_next) p->lru_next->lru_prev = p->lru_prev;
    else             lru_tail              = p->lru_prev;
    p->lru_prev = p->lru_next = 0;
}

// Move page to LRU head (most-recently-used)
static void _lru_touch(struct page *p) {
    if (lru_head == p) return;
    if (p->lru_prev || p->lru_next || lru_tail == p)
        _lru_remove(p);
    p->lru_prev = 0;
    p->lru_next = lru_head;
    if (lru_head) lru_head->lru_prev = p;
    lru_head    = p;
    if (!lru_tail) lru_tail = p;
}

// Write dirty page back to disk
static void _writeback(struct page *p) {
    if (!(p->flags & PC_FLAG_DIRTY)) return;
    uint32_t spb = p->block_size / 512;        // sectors per block
    uint32_t lba = p->block_no * spb;
    for (uint32_t i = 0; i < spb; i++)
        blkdev_write_sector(lba + i, p->data + i * 512);
    p->flags &= (uint8_t)~PC_FLAG_DIRTY;
    stat_writebacks++;
}

// Evict the LRU tail page that is not pinned; returns NULL if all are pinned
static struct page *_evict_one(void) {
    struct page *p = lru_tail;
    while (p) {
        if (p->pin_count == 0) {
            if (p->flags & PC_FLAG_DIRTY)
                _writeback(p);
            _lru_remove(p);
            _hash_remove(p);
            p->flags      = 0;
            p->pin_count  = 0;
            stat_evictions++;
            return p;
        }
        p = p->lru_prev;
    }
    return 0;
}

// Allocate a free page slot from the pool, evicting if necessary
static struct page *_alloc_page(void) {
    for (int i = 0; i < PC_MAX_PAGES; i++) {
        if (!(pool[i].flags & PC_FLAG_USED))
            return &pool[i];
    }
    return _evict_one();
}

// Initialise page cache: zero pool, clear stats, reset LRU
void pc_init(void) {
    memory_set(pool,       0, sizeof(pool));
    memory_set(hash_table, 0, sizeof(hash_table));
    lru_head        = 0;
    lru_tail        = 0;
    stat_hits       = 0;
    stat_misses     = 0;
    stat_evictions  = 0;
    stat_writebacks = 0;
    klog(LOG_OK, "Page cache initialized");
}

// Get a page from cache; on miss, read from disk into a new or evicted slot
uint8_t *pc_get_page(uint32_t dev, uint32_t block_no, uint32_t block_size) {
    struct page *p = _hash_find(dev, block_no);
    if (p) {
        stat_hits++;
        p->pin_count++;
        _lru_touch(p);
        return p->data;
    }

    stat_misses++;

    if (block_size > PC_MAX_BLOCK_SIZE) {
        kprint("[pc] pc_get_page: block_size exceeds PC_MAX_BLOCK_SIZE\n");
        return 0;
    }

    p = _alloc_page();
    if (!p) {
        kprint("[pc] pc_get_page: all pages pinned, cache full!\n");
        return 0;
    }

    // Lazy data allocation — pages start with NULL data until first use
    if (!p->data) {
        p->data = (uint8_t*)kalloc();
        if (!p->data) {
            kprint("[pc] pc_get_page: kalloc failed\n");
            return 0;
        }
    }

    p->dev        = dev;
    p->block_no   = block_no;
    p->block_size = block_size;
    p->flags      = PC_FLAG_USED;
    p->pin_count  = 1;

    uint32_t spb = block_size / 512;
    uint32_t lba = block_no * spb;
    memory_set(p->data, 0, block_size);
    for (uint32_t i = 0; i < spb; i++)
        blkdev_read_sector(lba + i, p->data + i * 512);
    p->flags |= PC_FLAG_VALID;

    _hash_insert(p);
    _lru_touch(p);

    return p->data;
}

// Mark page dirty and immediately write back (aggressive — see audit P-02)
void pc_mark_dirty(uint32_t dev, uint32_t block_no) {
    struct page *p = _hash_find(dev, block_no);
    if (!p) return;
    p->flags |= PC_FLAG_DIRTY;
    _writeback(p);
}

// Decrement pin count so page becomes eligible for eviction
void pc_put_page(uint32_t dev, uint32_t block_no) {
    struct page *p = _hash_find(dev, block_no);
    if (!p) return;
    if (p->pin_count > 0) p->pin_count--;
}

// Flush all dirty pages belonging to a device
void pc_flush_dev(uint32_t dev) {
    for (int i = 0; i < PC_MAX_PAGES; i++) {
        struct page *p = &pool[i];
        if ((p->flags & (PC_FLAG_VALID | PC_FLAG_DIRTY)) ==
                        (PC_FLAG_VALID | PC_FLAG_DIRTY) &&
            p->dev == dev) {
            _writeback(p);
        }
    }
}

// Remove a single block from cache, writing back first if dirty
void pc_invalidate_block(uint32_t dev, uint32_t block_no) {
    struct page *p = _hash_find(dev, block_no);
    if (!p) return;
    if (p->flags & PC_FLAG_DIRTY) _writeback(p);
    _lru_remove(p);
    _hash_remove(p);
    p->flags     = 0;
    p->pin_count = 0;
}

// Flush and remove all pages belonging to a device, freeing their data
void pc_invalidate_dev(uint32_t dev) {
    pc_flush_dev(dev);
    for (int i = 0; i < PC_MAX_PAGES; i++) {
        struct page *p = &pool[i];
        if ((p->flags & PC_FLAG_VALID) && p->dev == dev) {
            _lru_remove(p);
            _hash_remove(p);
            kfree_page(p->data);
            p->data      = 0;
            p->flags     = 0;
            p->pin_count = 0;
        }
    }
}

// Print cache statistics and current state
void pc_dump_stats(void) {
    kprint("[pc] hits=");      { char _b[16]; itoa((int)(stat_hits), _b); kprint(_b); };
    kprint(" misses=");        { char _b[16]; itoa((int)(stat_misses), _b); kprint(_b); };
    kprint(" evictions=");     { char _b[16]; itoa((int)(stat_evictions), _b); kprint(_b); };
    kprint(" writebacks=");    { char _b[16]; itoa((int)(stat_writebacks), _b); kprint(_b); };
    kprint("\n");

    uint32_t valid = 0, dirty = 0, pinned = 0;
    for (int i = 0; i < PC_MAX_PAGES; i++) {
        if (pool[i].flags & PC_FLAG_VALID)  valid++;
        if (pool[i].flags & PC_FLAG_DIRTY)  dirty++;
        if (pool[i].pin_count > 0)          pinned++;
    }
    kprint("[pc] pages: valid="); { char _b[16]; itoa((int)(valid), _b); kprint(_b); };
    kprint(" dirty=");            { char _b[16]; itoa((int)(dirty), _b); kprint(_b); };
    kprint(" pinned=");           { char _b[16]; itoa((int)(pinned), _b); kprint(_b); };
    kprint("/");                  { char _b[16]; itoa((int)(PC_MAX_PAGES), _b); kprint(_b); };
    kprint("\n");
}