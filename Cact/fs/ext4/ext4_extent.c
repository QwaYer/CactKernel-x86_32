#include "ext4_internal.h"
#include "klib.h"
#include "memory.h"

void ext4_extent_init(struct ext4_inode* inode) {
    memory_set(inode->i_block, 0, sizeof(inode->i_block));
    struct ext4_extent_header* eh = (struct ext4_extent_header*)inode->i_block;
    eh->eh_magic   = 0xF30A;
    eh->eh_entries = 0;
    eh->eh_max     = 4;
    eh->eh_depth   = 0;
}

uint32_t ext4_extent_pblock(struct ext4_inode* inode, uint32_t fb) {
    struct ext4_extent_header* eh = (struct ext4_extent_header*)inode->i_block;
    if (eh->eh_magic != 0xF30A || eh->eh_depth != 0) return 0;
    struct ext4_extent* ee = (struct ext4_extent*)((uint8_t*)inode->i_block + sizeof(*eh));
    for (uint16_t i = 0; i < eh->eh_entries; i++)
        if (fb >= ee[i].ee_block && fb < ee[i].ee_block + ee[i].ee_len)
            return ee[i].ee_start_lo + (fb - ee[i].ee_block);
    return 0;
}

int ext4_extent_add(struct ext4_inode* inode, uint32_t fb, uint32_t pb, uint16_t len) {
    struct ext4_extent_header* eh = (struct ext4_extent_header*)inode->i_block;
    if (eh->eh_entries >= eh->eh_max) return -1;
    struct ext4_extent* ee = (struct ext4_extent*)((uint8_t*)inode->i_block + sizeof(*eh));
    uint16_t idx         = eh->eh_entries;
    ee[idx].ee_block     = fb;
    ee[idx].ee_len       = len;
    ee[idx].ee_start_hi  = 0;
    ee[idx].ee_start_lo  = pb;
    eh->eh_entries++;
    return 0;
}

uint32_t ext4_legacy_bmap(struct ext4_ctx* ctx, struct ext4_inode* inode, uint32_t fb) {
    uint32_t ppb = ctx->block_size / 4;

    if (fb < 12) return inode->i_block[fb];

    fb -= 12;

    if (fb < ppb) {
        uint32_t ind = inode->i_block[12];
        if (!ind) return 0;
        uint32_t* buf = (uint32_t*)kmalloc(ctx->block_size);
        if (!buf) return 0;
        ext4_read_block(ctx, ind, (uint8_t*)buf);
        uint32_t pb = buf[fb];
        kfree_heap(buf);
        return pb;
    }

    fb -= ppb;

    if (fb < ppb * ppb) {
        uint32_t dind = inode->i_block[13];
        if (!dind) return 0;
        uint32_t* buf = (uint32_t*)kmalloc(ctx->block_size);
        if (!buf) return 0;
        ext4_read_block(ctx, dind, (uint8_t*)buf);
        uint32_t ind = buf[fb / ppb];
        if (!ind) {
            kfree_heap(buf);
            return 0;
        }
        ext4_read_block(ctx, ind, (uint8_t*)buf);
        uint32_t pb = buf[fb % ppb];
        kfree_heap(buf);
        return pb;
    }

    fb -= ppb * ppb;

    if (fb < ppb * ppb * ppb) {
        uint32_t tind = inode->i_block[14];
        if (!tind) return 0;
        uint32_t* buf = (uint32_t*)kmalloc(ctx->block_size);
        if (!buf) return 0;
        ext4_read_block(ctx, tind, (uint8_t*)buf);
        uint32_t dind = buf[fb / (ppb * ppb)];
        if (!dind) {
            kfree_heap(buf);
            return 0;
        }
        ext4_read_block(ctx, dind, (uint8_t*)buf);
        uint32_t ind = buf[(fb / ppb) % ppb];
        if (!ind) {
            kfree_heap(buf);
            return 0;
        }
        ext4_read_block(ctx, ind, (uint8_t*)buf);
        uint32_t pb = buf[fb % ppb];
        kfree_heap(buf);
        return pb;
    }

    return 0;
}
