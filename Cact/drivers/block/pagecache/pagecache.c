#include "pagecache.h"
#include "blkdev.h"
#include "kernel.h"
#include "memory.h"
#include "klib.h"
#include "sync.h"

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

// Synchronisation — protects hash table, LRU list, pool flags/pin_count
static irq_spinlock_t pc_lock;

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
    if (!p->data) {
        printk("[pc] _writeback: page has no data buffer, clearing dirty flag\n");
        p->flags &= (uint8_t)~PC_FLAG_DIRTY;
        return;
    }
    uint32_t spb = p->block_size / 512;        // sectors per block
    uint32_t lba = p->block_no * spb;
    for (uint32_t i = 0; i < spb; i++)
        blkdev_write_sector(lba + i, p->data + i * 512);
    p->flags &= (uint8_t)~PC_FLAG_DIRTY;
    stat_writebacks++;
}

// Write back page while holding it pinned to prevent concurrent eviction.
// Caller must hold pc_lock. Lock is released before _writeback and
// re-acquired after; caller must re-check invariants after return.
static void _writeback_pinned(struct page *p) {
    p->pin_count++;
    irq_spinlock_release(&pc_lock);
    _writeback(p);
    irq_spinlock_acquire(&pc_lock);
    p->pin_count--;
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

// Initialise page cache pool, hash table, LRU, and lock
void pc_init(void) {
    memory_set(pool,       0, sizeof(pool));
    memory_set(hash_table, 0, sizeof(hash_table));
    lru_head        = 0;
    lru_tail        = 0;
    stat_hits       = 0;
    stat_misses     = 0;
    stat_evictions  = 0;
    stat_writebacks = 0;
    irq_spinlock_init(&pc_lock);
    klog(LOG_OK, "Page cache initialized");
}

// Get a page from cache; on miss, read from disk into a new or evicted slot
uint8_t *pc_get_page(uint32_t dev, uint32_t block_no, uint32_t block_size) {
    irq_spinlock_acquire(&pc_lock);
    struct page *p = _hash_find(dev, block_no);
    if (p) {
        stat_hits++;
        p->pin_count++;
        _lru_touch(p);
        irq_spinlock_release(&pc_lock);
        return p->data;
    }
    stat_misses++;
    irq_spinlock_release(&pc_lock);

    if (block_size == 0 || block_size % 512 != 0) {
        printk("[pc] pc_get_page: block_size must be non-zero multiple of 512\n");
        return 0;
    }

    if (block_size > PC_MAX_BLOCK_SIZE) {
        printk("[pc] pc_get_page: block_size exceeds PC_MAX_BLOCK_SIZE\n");
        return 0;
    }

    uint32_t spb = block_size / 512;

    // Check LBA overflow and device bounds
    blkdev_t *bd = blkdev_get_boot();
    if (bd) {
        if (block_no > (UINT32_MAX / spb)) {
            printk("[pc] pc_get_page: LBA computation overflow\n");
            return 0;
        }
        uint32_t lba = block_no * spb;
        if (lba + spb > bd->max_lba) {
            printk("[pc] pc_get_page: LBA out of device range\n");
            return 0;
        }
    }

    irq_spinlock_acquire(&pc_lock);
    p = _alloc_page();
    if (!p) {
        irq_spinlock_release(&pc_lock);
        printk("[pc] pc_get_page: all pages pinned, cache full!\n");
        return 0;
    }
    p->dev        = dev;
    p->block_no   = block_no;
    p->block_size = block_size;
    p->flags      = PC_FLAG_USED;
    p->pin_count  = 1;
    irq_spinlock_release(&pc_lock);

    // Lazy data allocation — pages start with NULL data until first use
    if (!p->data) {
        p->data = (uint8_t*)kalloc();
        if (!p->data) {
            printk("[pc] pc_get_page: kalloc failed\n");
            irq_spinlock_acquire(&pc_lock);
            p->flags     = 0;
            p->pin_count = 0;
            irq_spinlock_release(&pc_lock);
            return 0;
        }
    }

    uint32_t lba = block_no * spb;
    memory_set(p->data, 0, block_size);
    for (uint32_t i = 0; i < spb; i++)
        blkdev_read_sector(lba + i, p->data + i * 512);

    irq_spinlock_acquire(&pc_lock);
    // Double-check: another thread may have inserted this block while we did I/O
    struct page *existing = _hash_find(dev, block_no);
    if (existing) {
        existing->pin_count++;
        _lru_touch(existing);
        irq_spinlock_release(&pc_lock);
        p->flags     = 0;
        p->pin_count = 0;
        return existing->data;
    }
    p->flags |= PC_FLAG_VALID;
    _hash_insert(p);
    _lru_touch(p);
    irq_spinlock_release(&pc_lock);

    return p->data;
}

// Mark page dirty and immediately write back.
// Pins the page temporarily to prevent TOCTOU eviction between lookup and writeback.
void pc_mark_dirty(uint32_t dev, uint32_t block_no) {
    irq_spinlock_acquire(&pc_lock);
    struct page *p = _hash_find(dev, block_no);
    if (!p) { irq_spinlock_release(&pc_lock); return; }
    p->flags |= PC_FLAG_DIRTY;
    _writeback_pinned(p);
    irq_spinlock_release(&pc_lock);
}

// Decrement pin count so page becomes eligible for eviction
void pc_put_page(uint32_t dev, uint32_t block_no) {
    irq_spinlock_acquire(&pc_lock);
    struct page *p = _hash_find(dev, block_no);
    if (!p) { irq_spinlock_release(&pc_lock); return; }
    if (p->pin_count == 0) {
        irq_spinlock_release(&pc_lock);
        printk("[pc] pc_put_page: pin_count underflow (dev=");
        { char _b[16]; itoa((int)dev, _b); printk(_b); }
        printk(", block=");
        { char _b[16]; itoa((int)block_no, _b); printk(_b); }
        printk(")\n");
        return;
    }
    p->pin_count--;
    irq_spinlock_release(&pc_lock);
}

// Flush all dirty pages belonging to a device
void pc_flush_dev(uint32_t dev) {
    irq_spinlock_acquire(&pc_lock);
    for (int i = 0; i < PC_MAX_PAGES; i++) {
        struct page *p = &pool[i];
        if ((p->flags & (PC_FLAG_VALID | PC_FLAG_DIRTY)) ==
                        (PC_FLAG_VALID | PC_FLAG_DIRTY) &&
            p->dev == dev) {
            _writeback_pinned(p);
        }
    }
    irq_spinlock_release(&pc_lock);
}

// Remove a single block from cache, writing back first if dirty.
// Refuses to invalidate pinned pages — caller must unpin first.
// Frees the data buffer to prevent use-after-free on reallocation.
void pc_invalidate_block(uint32_t dev, uint32_t block_no) {
    uint8_t *freed_data;
    irq_spinlock_acquire(&pc_lock);
    struct page *p = _hash_find(dev, block_no);
    if (!p) { irq_spinlock_release(&pc_lock); return; }
    if (p->pin_count > 0) { irq_spinlock_release(&pc_lock); return; }
    if (p->flags & PC_FLAG_DIRTY)
        _writeback_pinned(p);
    _lru_remove(p);
    _hash_remove(p);
    freed_data = p->data;
    p->data      = 0;
    p->flags     = 0;
    p->pin_count = 0;
    irq_spinlock_release(&pc_lock);

    if (freed_data)
        free_page(freed_data);
}

// Flush and remove all pages belonging to a device, freeing their data.
// Skips pages that are still pinned.
void pc_invalidate_dev(uint32_t dev) {
    pc_flush_dev(dev);
    irq_spinlock_acquire(&pc_lock);
    for (int i = 0; i < PC_MAX_PAGES; i++) {
        struct page *p = &pool[i];
        if ((p->flags & PC_FLAG_VALID) && p->dev == dev) {
            if (p->pin_count > 0) continue;
            _lru_remove(p);
            _hash_remove(p);
            uint8_t *freed_data = p->data;
            p->data      = 0;
            p->flags     = 0;
            p->pin_count = 0;
            irq_spinlock_release(&pc_lock);
            if (freed_data)
                free_page(freed_data);
            irq_spinlock_acquire(&pc_lock);
        }
    }
    irq_spinlock_release(&pc_lock);
}

// Print cache statistics and current state
void pc_dump_stats(void) {
    irq_spinlock_acquire(&pc_lock);
    printk("[pc] hits=");      { char _b[16]; itoa((int)(stat_hits), _b); printk(_b); };
    printk(" misses=");        { char _b[16]; itoa((int)(stat_misses), _b); printk(_b); };
    printk(" evictions=");     { char _b[16]; itoa((int)(stat_evictions), _b); printk(_b); };
    printk(" writebacks=");    { char _b[16]; itoa((int)(stat_writebacks), _b); printk(_b); };
    printk("\n");

    uint32_t valid = 0, dirty = 0, pinned = 0;
    for (int i = 0; i < PC_MAX_PAGES; i++) {
        if (pool[i].flags & PC_FLAG_VALID)  valid++;
        if (pool[i].flags & PC_FLAG_DIRTY)  dirty++;
        if (pool[i].pin_count > 0)          pinned++;
    }
    irq_spinlock_release(&pc_lock);
    printk("[pc] pages: valid="); { char _b[16]; itoa((int)(valid), _b); printk(_b); };
    printk(" dirty=");            { char _b[16]; itoa((int)(dirty), _b); printk(_b); };
    printk(" pinned=");           { char _b[16]; itoa((int)(pinned), _b); printk(_b); };
    printk("/");                  { char _b[16]; itoa((int)(PC_MAX_PAGES), _b); printk(_b); };
    printk("\n");
}