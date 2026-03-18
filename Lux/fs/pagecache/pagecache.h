#ifndef PAGECACHE_H
#define PAGECACHE_H

#include <stdint.h>

#define PC_MAX_PAGES       64
#define PC_MAX_BLOCK_SIZE  4096

#define PC_HASH_SIZE       64

#define PC_FLAG_VALID   0x01
#define PC_FLAG_DIRTY   0x02
#define PC_FLAG_USED    0x04

struct page;

void pc_init(void);

uint8_t *pc_get_page(uint32_t dev, uint32_t block_no, uint32_t block_size);

void pc_mark_dirty(uint32_t dev, uint32_t block_no);

void pc_put_page(uint32_t dev, uint32_t block_no);

void pc_flush_dev(uint32_t dev);

void pc_invalidate_block(uint32_t dev, uint32_t block_no);

void pc_invalidate_dev(uint32_t dev);

void pc_dump_stats(void);

#endif
