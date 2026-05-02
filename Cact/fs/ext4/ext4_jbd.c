#include "ext4_internal.h"
#include "blkdev.h"
#include "klib.h"
#include "memory.h"
#include "pagecache.h"

static void ext4_jbd_write_phys(struct ext4_ctx* ctx, uint32_t block, uint8_t* buf) {
    uint32_t spb = ctx->block_size / 512;
    uint32_t lba = block * spb;
    for (uint32_t i = 0; i < spb; i++)
        blkdev_write_sector(lba + i, buf + i * 512);
}

static uint32_t ext4_jbd_write_via_inode(struct ext4_ctx* ctx, uint32_t jblock, uint8_t* buf) {
    struct ext4_inode ji;
    ext4_read_inode(ctx, ctx->journal.j_inum, &ji);

    struct ext4_extent_header* eh = (struct ext4_extent_header*)ji.i_block;
    if (eh->eh_magic == 0xF30A) {
        struct ext4_extent* ee =
            (struct ext4_extent*)((uint8_t*)ji.i_block + sizeof(*eh));
        for (uint16_t i = 0; i < eh->eh_entries; i++) {
            if (jblock >= ee[i].ee_block && jblock < ee[i].ee_block + ee[i].ee_len) {
                uint32_t phys = ee[i].ee_start_lo + (jblock - ee[i].ee_block);
                ext4_jbd_write_phys(ctx, phys, buf);
                return 1;
            }
        }
    } else {
        if (jblock < 12 && ji.i_block[jblock]) {
            ext4_jbd_write_phys(ctx, ji.i_block[jblock], buf);
            return 1;
        }
    }
    return 0;
}

static void ext4_journal_commit(struct ext4_ctx* ctx) {
    struct jbd2_journal* j = &ctx->journal;
    if (!j->j_sb || !j->j_running_transaction) return;
    if (j->j_running_transaction->t_nr_buffers == 0) return;

    struct jbd2_transaction* t = j->j_running_transaction;
    uint32_t start = j->j_head;

    uint8_t* dbuf = (uint8_t*)kmalloc(ctx->block_size);
    if (!dbuf) return;
    memory_set(dbuf, 0, ctx->block_size);

    struct jbd2_header* hdr = (struct jbd2_header*)dbuf;
    hdr->h_magic     = JBD2_MAGIC_NUMBER;
    hdr->h_blocktype = JBD2_DESCRIPTOR_BLOCK;
    hdr->h_sequence  = t->t_tid;

    uint32_t max_tags = (ctx->block_size - sizeof(struct jbd2_header)) / sizeof(struct jbd2_block_tag);
    struct jbd2_block_tag* tag = (struct jbd2_block_tag*)(dbuf + sizeof(struct jbd2_header));

    struct jbd2_buffer* b = t->t_buffers;
    uint32_t cur = start + 1;
    if (cur >= j->j_maxlen) cur = j->j_first;

    uint32_t tc = 0;
    while (b && tc < max_tags) {
        tag->t_blocknr = b->b_blocknr;
        tag->t_flags   = (!b->b_next || tc + 1 >= max_tags) ? 8 : 0;
        ext4_jbd_write_via_inode(ctx, cur, b->b_data);
        cur++;
        if (cur >= j->j_maxlen) cur = j->j_first;
        tag++; tc++; b = b->b_next;
    }

    ext4_jbd_write_via_inode(ctx, start, dbuf);
    kfree_heap(dbuf);

    uint8_t* cbuf = (uint8_t*)kmalloc(ctx->block_size);
    if (!cbuf) return;
    memory_set(cbuf, 0, ctx->block_size);
    hdr = (struct jbd2_header*)cbuf;
    hdr->h_magic     = JBD2_MAGIC_NUMBER;
    hdr->h_blocktype = JBD2_COMMIT_BLOCK;
    hdr->h_sequence  = t->t_tid;
    ext4_jbd_write_via_inode(ctx, cur, cbuf);
    kfree_heap(cbuf);

    cur++;
    if (cur >= j->j_maxlen) cur = j->j_first;
    j->j_head = cur;
    j->j_sb->s_start    = j->j_head;
    j->j_sb->s_sequence = t->t_tid + 1;

    uint8_t* jsb = (uint8_t*)kmalloc(ctx->block_size);
    if (jsb) {
        memory_set(jsb, 0, ctx->block_size);
        memory_copy(jsb, j->j_sb, sizeof(struct jbd2_superblock));
        ext4_jbd_write_via_inode(ctx, 0, jsb);
        kfree_heap(jsb);
    }
}

void ext4_journal_start(struct ext4_ctx* ctx) {
    struct jbd2_journal* j = &ctx->journal;
    if (!j->j_sb || j->j_running_transaction) return;
    struct jbd2_transaction* t = (struct jbd2_transaction*)kmalloc(sizeof(*t));
    if (!t) return;
    t->t_tid        = j->j_sb->s_sequence++;
    t->t_state      = 1;
    t->t_nr_buffers = 0;
    t->t_buffers    = 0;
    j->j_running_transaction = t;
}

void ext4_journal_stop(struct ext4_ctx* ctx) {
    struct jbd2_journal* j = &ctx->journal;
    if (!j->j_sb || !j->j_running_transaction) return;
    ext4_journal_commit(ctx);
    pc_flush_dev(ctx->dev);
    struct jbd2_buffer* b = j->j_running_transaction->t_buffers;
    while (b) {
        struct jbd2_buffer* nx = b->b_next;
        kfree_heap(b->b_data);
        kfree_heap(b);
        b = nx;
    }
    kfree_heap(j->j_running_transaction);
    j->j_running_transaction = 0;
}

void ext4_journal_log(struct ext4_ctx* ctx, uint32_t blocknr, uint8_t* data) {
    struct jbd2_journal* j = &ctx->journal;
    if (!j->j_sb || !j->j_running_transaction) return;
    struct jbd2_buffer* b = j->j_running_transaction->t_buffers;
    while (b) {
        if (b->b_blocknr == blocknr) {
            memory_copy(b->b_data, data, ctx->block_size);
            return;
        }
        b = b->b_next;
    }
    b = (struct jbd2_buffer*)kmalloc(sizeof(*b));
    if (!b) return;
    b->b_blocknr = blocknr;
    b->b_data    = (uint8_t*)kmalloc(ctx->block_size);
    if (!b->b_data) {
        kfree_heap(b);
        return;
    }
    memory_copy(b->b_data, data, ctx->block_size);
    b->b_next = j->j_running_transaction->t_buffers;
    j->j_running_transaction->t_buffers = b;
    j->j_running_transaction->t_nr_buffers++;
}

uint32_t ext4_jbd_read(struct ext4_ctx* ctx, uint32_t jblock, uint8_t* buf) {
    struct ext4_inode ji;
    ext4_read_inode(ctx, ctx->journal.j_inum, &ji);

    struct ext4_extent_header* eh = (struct ext4_extent_header*)ji.i_block;
    if (eh->eh_magic == 0xF30A) {
        struct ext4_extent* ee =
            (struct ext4_extent*)((uint8_t*)ji.i_block + sizeof(*eh));
        for (uint16_t i = 0; i < eh->eh_entries; i++) {
            if (jblock >= ee[i].ee_block && jblock < ee[i].ee_block + ee[i].ee_len) {
                ext4_read_block(ctx, ee[i].ee_start_lo + (jblock - ee[i].ee_block), buf);
                return 1;
            }
        }
    } else {
        if (jblock < 12 && ji.i_block[jblock]) {
            ext4_read_block(ctx, ji.i_block[jblock], buf);
            return 1;
        }
    }
    return 0;
}
