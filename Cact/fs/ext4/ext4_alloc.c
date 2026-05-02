#include "ext4_internal.h"
#include "klib.h"
#include "memory.h"


// Bitmap helpers: test, set, clear a single bit in a byte array
static int  ext4_btest(uint8_t* bm, uint32_t b) { return (bm[b / 8] >> (b % 8)) & 1; }
static void ext4_bset(uint8_t* bm, uint32_t b) { bm[b / 8] |= (uint8_t)(1 << (b % 8)); }
static void ext4_bclr(uint8_t* bm, uint32_t b) { bm[b / 8] &= (uint8_t) ~(1 << (b % 8)); }


// Allocate a single free block within a specific block group.
// Returns absolute block number, or 0 if group is full.
static uint32_t ext4_alloc_block_group(struct ext4_ctx* ctx, uint32_t group) {
    struct ext4_group_desc gd;
    ext4_read_gd(ctx, group, &gd);
    if (!gd.bg_free_blocks_count_lo) return 0;

    uint8_t* bm = (uint8_t*)kmalloc(ctx->block_size);
    if (!bm) return 0;
    memory_set(bm, 0, ctx->block_size);
    ext4_read_block(ctx, gd.bg_block_bitmap_lo, bm);

    uint32_t found = 0, ok = 0;
    for (uint32_t i = 0; i < ctx->sb.s_blocks_per_group; i++) {
        if (!ext4_btest(bm, i)) {
            ext4_bset(bm, i);
            found = i;
            ok    = 1;
            break;
        }
    }
    if (!ok) { kfree_heap(bm); return 0; }

    ext4_write_block(ctx, gd.bg_block_bitmap_lo, bm);
    kfree_heap(bm);

    gd.bg_free_blocks_count_lo--;
    ext4_write_gd(ctx, group, &gd);
    ctx->sb.s_free_blocks_count_lo--;
    ext4_write_sb(ctx);

    return ctx->sb.s_first_data_block + group * ctx->sb.s_blocks_per_group + found;
}

// Allocate a free block from any group (linear scan).
uint32_t ext4_alloc_block(struct ext4_ctx* ctx) {
    uint32_t groups =
        (ctx->sb.s_blocks_count_lo + ctx->sb.s_blocks_per_group - 1) / ctx->sb.s_blocks_per_group;
    for (uint32_t g = 0; g < groups; g++) {
        uint32_t b = ext4_alloc_block_group(ctx, g);
        if (b) return b;
    }
    return 0;
}

// Free a previously allocated block back to its group bitmap.
void ext4_free_block(struct ext4_ctx* ctx, uint32_t block) {
    if (!block) return;
    uint32_t rel   = block - ctx->sb.s_first_data_block;
    uint32_t group = rel / ctx->sb.s_blocks_per_group;
    uint32_t index = rel % ctx->sb.s_blocks_per_group;

    struct ext4_group_desc gd;
    ext4_read_gd(ctx, group, &gd);

    uint8_t* bm = (uint8_t*)kmalloc(ctx->block_size);
    if (!bm) return;
    memory_set(bm, 0, ctx->block_size);
    ext4_read_block(ctx, gd.bg_block_bitmap_lo, bm);

    ext4_bclr(bm, index);
    ext4_write_block(ctx, gd.bg_block_bitmap_lo, bm);
    kfree_heap(bm);

    gd.bg_free_blocks_count_lo++;
    ext4_write_gd(ctx, group, &gd);
    ctx->sb.s_free_blocks_count_lo++;
    ext4_write_sb(ctx);
}


// Allocate a single free inode within a specific block group.
// Returns absolute inode number, or 0 if group is full.
static uint32_t ext4_alloc_inode_group(struct ext4_ctx* ctx, uint32_t group) {
    struct ext4_group_desc gd;
    ext4_read_gd(ctx, group, &gd);
    if (!gd.bg_free_inodes_count_lo) return 0;

    uint8_t* bm = (uint8_t*)kmalloc(ctx->block_size);
    if (!bm) return 0;
    memory_set(bm, 0, ctx->block_size);
    ext4_read_block(ctx, gd.bg_inode_bitmap_lo, bm);

    uint32_t found = 0, ok = 0;
    for (uint32_t i = 0; i < ctx->sb.s_inodes_per_group; i++) {
        if (!ext4_btest(bm, i)) {
            ext4_bset(bm, i);
            found = i;
            ok    = 1;
            break;
        }
    }
    if (!ok) { kfree_heap(bm); return 0; }

    ext4_write_block(ctx, gd.bg_inode_bitmap_lo, bm);
    kfree_heap(bm);

    gd.bg_free_inodes_count_lo--;
    ext4_write_gd(ctx, group, &gd);
    ctx->sb.s_free_inodes_count--;
    ext4_write_sb(ctx);

    return group * ctx->sb.s_inodes_per_group + found + 1;
}

// Allocate a free inode from any group (linear scan).
uint32_t ext4_alloc_inode(struct ext4_ctx* ctx) {
    uint32_t groups =
        (ctx->sb.s_blocks_count_lo + ctx->sb.s_blocks_per_group - 1) / ctx->sb.s_blocks_per_group;
    for (uint32_t g = 0; g < groups; g++) {
        uint32_t ino = ext4_alloc_inode_group(ctx, g);
        if (ino) return ino;
    }
    return 0;
}

// Free a previously allocated inode back to its group bitmap.
void ext4_free_inode(struct ext4_ctx* ctx, uint32_t ino) {
    if (!ino) return;
    uint32_t group = (ino - 1) / ctx->sb.s_inodes_per_group;
    uint32_t index = (ino - 1) % ctx->sb.s_inodes_per_group;

    struct ext4_group_desc gd;
    ext4_read_gd(ctx, group, &gd);

    uint8_t* bm = (uint8_t*)kmalloc(ctx->block_size);
    if (!bm) return;
    memory_set(bm, 0, ctx->block_size);
    ext4_read_block(ctx, gd.bg_inode_bitmap_lo, bm);

    ext4_bclr(bm, index);
    ext4_write_block(ctx, gd.bg_inode_bitmap_lo, bm);
    kfree_heap(bm);

    gd.bg_free_inodes_count_lo++;
    ext4_write_gd(ctx, group, &gd);
    ctx->sb.s_free_inodes_count++;
    ext4_write_sb(ctx);
}