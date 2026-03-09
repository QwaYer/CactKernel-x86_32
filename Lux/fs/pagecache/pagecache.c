#include "pagecache.h"
#include "ata.h"
#include "kernel.h"
#include "memory.h"


struct page {
    uint16_t  port;
    uint8_t   slave;
    uint8_t   flags;          
    uint32_t  block_no;
    uint32_t  block_size;     

    int       pin_count;

    struct page *hash_next;

    struct page *lru_prev;
    struct page *lru_next;

    uint8_t  *data;
};


static struct page   pool[PC_MAX_PAGES];
static struct page  *hash_table[PC_HASH_SIZE];

static struct page  *lru_head; 
static struct page  *lru_tail;   

static uint32_t stat_hits;
static uint32_t stat_misses;
static uint32_t stat_evictions;
static uint32_t stat_writebacks;

static inline uint32_t _hash(uint16_t port, uint8_t slave, uint32_t block_no) {
    uint32_t h = (uint32_t)port ^ ((uint32_t)slave << 16);
    h ^= block_no;
    h ^= h >> 16;
    h *= 0x45d9f3bU;
    h ^= h >> 16;
    return h & (PC_HASH_SIZE - 1);
}

static void _hash_remove(struct page *p) {
    uint32_t bucket = _hash(p->port, p->slave, p->block_no);
    struct page **pp = &hash_table[bucket];
    while (*pp) {
        if (*pp == p) { *pp = p->hash_next; p->hash_next = 0; return; }
        pp = &(*pp)->hash_next;
    }
}

static void _hash_insert(struct page *p) {
    uint32_t bucket = _hash(p->port, p->slave, p->block_no);
    p->hash_next         = hash_table[bucket];
    hash_table[bucket]   = p;
}

static struct page *_hash_find(uint16_t port, uint8_t slave, uint32_t block_no) {
    uint32_t bucket = _hash(port, slave, block_no);
    struct page *p  = hash_table[bucket];
    while (p) {
        if (p->port == port && p->slave == slave && p->block_no == block_no)
            return p;
        p = p->hash_next;
    }
    return 0;
}

static void _lru_remove(struct page *p) {
    if (p->lru_prev) p->lru_prev->lru_next = p->lru_next;
    else             lru_head              = p->lru_next;  
    if (p->lru_next) p->lru_next->lru_prev = p->lru_prev;
    else             lru_tail              = p->lru_prev;   
    p->lru_prev = p->lru_next = 0;
}

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

static void _writeback(struct page *p) {
    if (!(p->flags & PC_FLAG_DIRTY)) return;
    uint32_t spb = p->block_size / 512;
    uint32_t lba = p->block_no * spb;
    for (uint32_t i = 0; i < spb; i++)
        ata_write_sector(p->port, p->slave, lba + i, p->data + i * 512);
    p->flags &= (uint8_t)~PC_FLAG_DIRTY;
    stat_writebacks++;
}

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

static struct page *_alloc_page(void) {
    for (int i = 0; i < PC_MAX_PAGES; i++) {
        if (!(pool[i].flags & PC_FLAG_USED))
            return &pool[i];
    }
    return _evict_one();
}


//Public api
void pc_init(void) {
    memory_set(pool,       0, sizeof(pool));
    memory_set(hash_table, 0, sizeof(hash_table));
    lru_head      = 0;
    lru_tail      = 0;
    stat_hits     = 0;
    stat_misses   = 0;
    stat_evictions = 0;
    stat_writebacks = 0;


}

uint8_t *pc_get_page(uint16_t port, uint8_t slave,
                     uint32_t block_no, uint32_t block_size) {
    struct page *p = _hash_find(port, slave, block_no);
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

    if (!p->data) {
        p->data = (uint8_t*)kalloc();
        if (!p->data) {
            kprint("[pc] pc_get_page: kalloc failed\n");
            return 0;
        }
    }

    p->port       = port;
    p->slave      = slave;
    p->block_no   = block_no;
    p->block_size = block_size;
    p->flags      = PC_FLAG_USED;
    p->pin_count  = 1;

    uint32_t spb = block_size / 512;
    uint32_t lba = block_no * spb;
    memory_set(p->data, 0, block_size);
    for (uint32_t i = 0; i < spb; i++)
        ata_read_sector(port, slave, lba + i, p->data + i * 512);
    p->flags |= PC_FLAG_VALID;

    _hash_insert(p);
    _lru_touch(p);

    return p->data;
}

void pc_mark_dirty(uint16_t port, uint8_t slave, uint32_t block_no) {
    struct page *p = _hash_find(port, slave, block_no);
    if (p) p->flags |= PC_FLAG_DIRTY;
}

void pc_put_page(uint16_t port, uint8_t slave, uint32_t block_no) {
    struct page *p = _hash_find(port, slave, block_no);
    if (!p) return;
    if (p->pin_count > 0) p->pin_count--;
}

void pc_flush_dev(uint16_t port, uint8_t slave) {
    for (int i = 0; i < PC_MAX_PAGES; i++) {
        struct page *p = &pool[i];
        if ((p->flags & (PC_FLAG_VALID | PC_FLAG_DIRTY)) ==
                        (PC_FLAG_VALID | PC_FLAG_DIRTY) &&
            p->port == port && p->slave == slave) {
            _writeback(p);
        }
    }
}

void pc_invalidate_block(uint16_t port, uint8_t slave, uint32_t block_no) {
    struct page *p = _hash_find(port, slave, block_no);
    if (!p) return;
    if (p->flags & PC_FLAG_DIRTY) _writeback(p);
    _lru_remove(p);
    _hash_remove(p);
    p->flags     = 0;
    p->pin_count = 0;
}

void pc_invalidate_dev(uint16_t port, uint8_t slave) {
    pc_flush_dev(port, slave);
    for (int i = 0; i < PC_MAX_PAGES; i++) {
        struct page *p = &pool[i];
        if ((p->flags & PC_FLAG_VALID) &&
            p->port == port && p->slave == slave) {
            _lru_remove(p);
            _hash_remove(p);
            kfree_page(p->data);
            p->data      = 0;
            p->flags     = 0;
            p->pin_count = 0;
        }
    }
}

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