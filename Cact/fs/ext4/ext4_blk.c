#include "ext4_internal.h"
#include "blkdev.h"
#include "klib.h"
#include "memory.h"
#include "kernel.h"
#include "pagecache.h"

// Read a block (size = ctx->block_size) through the page cache with fallback to raw I/O
void ext4_read_block(struct ext4_ctx* ctx, uint32_t block, uint8_t* buf) {
    uint8_t* page = pc_get_page(ctx->dev, block, ctx->block_size);
    if (page) {
        memory_copy(buf, page, ctx->block_size);
        pc_put_page(ctx->dev, block);
    } else {
        kprint("[ext4] _read_block: page cache miss fallback, block=");
        uint32_t spb = ctx->block_size / 512;
        uint32_t lba = block * spb;
        for (uint32_t i = 0; i < spb; i++)
            blkdev_read_sector(lba + i, buf + i * 512);
    }
}

// Write a block, logging to journal first if active, then through page cache with fallback
void ext4_write_block(struct ext4_ctx* ctx, uint32_t block, uint8_t* buf) {
    // Log to journal before modifying the primary location
    ext4_journal_log(ctx, block, buf);

    uint8_t* page = pc_get_page(ctx->dev, block, ctx->block_size);
    if (page) {
        memory_copy(page, buf, ctx->block_size);
        pc_mark_dirty(ctx->dev, block);    // immediate writeback through page cache
        pc_put_page(ctx->dev, block);
    } else {
        uint32_t spb = ctx->block_size / 512;
        uint32_t lba = block * spb;
        for (uint32_t i = 0; i < spb; i++)
            blkdev_write_sector(lba + i, buf + i * 512);
    }
}

// Write superblock to sectors 2 and 3 (primary copy, 1024 bytes)
void ext4_write_sb(struct ext4_ctx* ctx) {
    uint8_t buf[1024];
    memory_set(buf, 0, sizeof(buf));
    memory_copy(buf, &ctx->sb, sizeof(struct ext4_superblock));
    blkdev_write_sector(2, buf);
    blkdev_write_sector(3, buf + 512);
}

// Compute first block of group descriptor table (block 1 or 2 depending on block size)
uint32_t ext4_gd_base(struct ext4_ctx* ctx) {
    return (ctx->block_size == 1024) ? 2 : 1;
}

// Read a group descriptor from disk (supports 32-byte and 64-byte descriptors)
void ext4_read_gd(struct ext4_ctx* ctx, uint32_t group, struct ext4_group_desc* gd) {
    uint32_t ds  = ctx->sb.s_desc_size ? ctx->sb.s_desc_size : 32;
    uint32_t off = group * ds;
    uint8_t* buf = (uint8_t*)kmalloc(ctx->block_size);
    if (!buf) return;
    memory_set(buf, 0, ctx->block_size);
    ext4_read_block(ctx, ext4_gd_base(ctx) + off / ctx->block_size, buf);
    memory_copy(gd, buf + off % ctx->block_size, sizeof(*gd));
    kfree_heap(buf);
}

// Write a group descriptor back to disk (read-modify-write within the containing block)
void ext4_write_gd(struct ext4_ctx* ctx, uint32_t group, struct ext4_group_desc* gd) {
    uint32_t ds  = ctx->sb.s_desc_size ? ctx->sb.s_desc_size : 32;
    uint32_t off = group * ds;
    uint8_t* buf = (uint8_t*)kmalloc(ctx->block_size);
    if (!buf) return;
    memory_set(buf, 0, ctx->block_size);
    ext4_read_block(ctx, ext4_gd_base(ctx) + off / ctx->block_size, buf);
    memory_copy(buf + off % ctx->block_size, gd, sizeof(*gd));
    ext4_write_block(ctx, ext4_gd_base(ctx) + off / ctx->block_size, buf);
    kfree_heap(buf);
}

// Read an inode from its group's inode table
void ext4_read_inode(struct ext4_ctx* ctx, uint32_t ino, struct ext4_inode* out) {
    if (!ino) return;
    struct ext4_group_desc gd;
    uint32_t group = (ino - 1) / ctx->sb.s_inodes_per_group;
    uint32_t index = (ino - 1) % ctx->sb.s_inodes_per_group;
    ext4_read_gd(ctx, group, &gd);

    uint32_t off = index * ctx->sb.s_inode_size;
    uint8_t* buf = (uint8_t*)kmalloc(ctx->block_size);
    if (!buf) return;
    memory_set(buf, 0, ctx->block_size);
    ext4_read_block(ctx, gd.bg_inode_table_lo + off / ctx->block_size, buf);
    memory_copy(out, buf + off % ctx->block_size, sizeof(*out));
    kfree_heap(buf);
}

// Write an inode back to its group's inode table (read-modify-write within the containing block)
void ext4_write_inode(struct ext4_ctx* ctx, uint32_t ino, struct ext4_inode* in) {
    if (!ino) return;
    struct ext4_group_desc gd;
    uint32_t group = (ino - 1) / ctx->sb.s_inodes_per_group;
    uint32_t index = (ino - 1) % ctx->sb.s_inodes_per_group;
    ext4_read_gd(ctx, group, &gd);

    uint32_t off = index * ctx->sb.s_inode_size;
    uint8_t* buf = (uint8_t*)kmalloc(ctx->block_size);
    if (!buf) return;
    memory_set(buf, 0, ctx->block_size);
    ext4_read_block(ctx, gd.bg_inode_table_lo + off / ctx->block_size, buf);
    memory_copy(buf + off % ctx->block_size, in, sizeof(*in));
    ext4_write_block(ctx, gd.bg_inode_table_lo + off / ctx->block_size, buf);
    kfree_heap(buf);
}