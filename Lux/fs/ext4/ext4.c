#include "ext4.h"
#include "nvme.h"
#include "libc.h"
#include "memory.h"
#include "kernel.h"
#include "pagecache.h"

static void _make_node(struct ext4_ctx* ctx, vfs_node_t* n, uint32_t ino, uint8_t ft);

static void _read_block(struct ext4_ctx* ctx, uint32_t block, uint8_t* buf) {
    uint8_t* page = pc_get_page(ctx->port, ctx->slave, block, ctx->block_size);
    if (page) {
        memory_copy(buf, page, ctx->block_size);
        pc_put_page(ctx->port, ctx->slave, block);
    } else {
        kprint("[ext4] _read_block: page cache miss fallback, block=");
        uint32_t spb = ctx->block_size / 512;
        uint32_t lba = block * spb;
        for (uint32_t i = 0; i < spb; i++)
            nvme_read_sector(lba + i, buf + i * 512);
    }
}

static void _write_block(struct ext4_ctx* ctx, uint32_t block, uint8_t* buf);


static uint32_t _jread(struct ext4_ctx* ctx, uint32_t jblock, uint8_t* buf) {
    struct ext4_inode ji;
    uint32_t group = (ctx->journal.j_inum - 1) / ctx->sb.s_inodes_per_group;
    uint32_t index = (ctx->journal.j_inum - 1) % ctx->sb.s_inodes_per_group;
    struct ext4_group_desc gd;
    uint8_t* gb = (uint8_t*)kmalloc(ctx->block_size);
    if (!gb) return 0;
    memory_set(gb, 0, ctx->block_size);
    uint32_t desc_size = ctx->sb.s_desc_size ? ctx->sb.s_desc_size : 32;
    uint32_t gd_base   = (ctx->block_size == 1024) ? 2 : 1;
    _read_block(ctx, gd_base + (group * desc_size) / ctx->block_size, gb);
    memory_copy(&gd, gb + (group * desc_size) % ctx->block_size, sizeof(gd));
    kfree_heap(gb);

    uint32_t off_b = ((index * ctx->sb.s_inode_size) / ctx->block_size);
    uint32_t off_i = ((index * ctx->sb.s_inode_size) % ctx->block_size);
    uint8_t* ib = (uint8_t*)kmalloc(ctx->block_size);
    if (!ib) return 0;
    _read_block(ctx, gd.bg_inode_table_lo + off_b, ib);
    memory_copy(&ji, ib + off_i, sizeof(ji));
    kfree_heap(ib);

    struct ext4_extent_header* eh = (struct ext4_extent_header*)ji.i_block;
    if (eh->eh_magic == 0xF30A) {
        struct ext4_extent* ee = (struct ext4_extent*)((uint8_t*)ji.i_block + sizeof(*eh));
        for (uint16_t i = 0; i < eh->eh_entries; i++) {
            if (jblock >= ee[i].ee_block && jblock < ee[i].ee_block + ee[i].ee_len) {
                _read_block(ctx, ee[i].ee_start_lo + (jblock - ee[i].ee_block), buf);
                return 1;
            }
        }
    } else {
        if (jblock < 12 && ji.i_block[jblock]) {
            _read_block(ctx, ji.i_block[jblock], buf);
            return 1;
        }
    }
    return 0;
}

static void _jwrite_phys(struct ext4_ctx* ctx, uint32_t block, uint8_t* buf) {
    uint32_t spb = ctx->block_size / 512;
    uint32_t lba = block * spb;
    for (uint32_t i = 0; i < spb; i++)
        nvme_write_sector(lba + i, buf + i * 512);
}

static uint32_t _jwrite(struct ext4_ctx* ctx, uint32_t jblock, uint8_t* buf) {
    struct ext4_inode ji;
    uint32_t group = (ctx->journal.j_inum - 1) / ctx->sb.s_inodes_per_group;
    uint32_t index = (ctx->journal.j_inum - 1) % ctx->sb.s_inodes_per_group;
    struct ext4_group_desc gd;
    uint8_t* gb = (uint8_t*)kmalloc(ctx->block_size);
    if (!gb) return 0;
    memory_set(gb, 0, ctx->block_size);
    uint32_t desc_size = ctx->sb.s_desc_size ? ctx->sb.s_desc_size : 32;
    uint32_t gd_base   = (ctx->block_size == 1024) ? 2 : 1;
    _read_block(ctx, gd_base + (group * desc_size) / ctx->block_size, gb);
    memory_copy(&gd, gb + (group * desc_size) % ctx->block_size, sizeof(gd));
    kfree_heap(gb);

    uint32_t off_b = ((index * ctx->sb.s_inode_size) / ctx->block_size);
    uint32_t off_i = ((index * ctx->sb.s_inode_size) % ctx->block_size);
    uint8_t* ib = (uint8_t*)kmalloc(ctx->block_size);
    if (!ib) return 0;
    _read_block(ctx, gd.bg_inode_table_lo + off_b, ib);
    memory_copy(&ji, ib + off_i, sizeof(ji));
    kfree_heap(ib);

    struct ext4_extent_header* eh = (struct ext4_extent_header*)ji.i_block;
    if (eh->eh_magic == 0xF30A) {
        struct ext4_extent* ee = (struct ext4_extent*)((uint8_t*)ji.i_block + sizeof(*eh));
        for (uint16_t i = 0; i < eh->eh_entries; i++) {
            if (jblock >= ee[i].ee_block && jblock < ee[i].ee_block + ee[i].ee_len) {
                uint32_t phys = ee[i].ee_start_lo + (jblock - ee[i].ee_block);
                _jwrite_phys(ctx, phys, buf);
                return 1;
            }
        }
    } else {
        if (jblock < 12 && ji.i_block[jblock]) {
            _jwrite_phys(ctx, ji.i_block[jblock], buf);
            return 1;
        }
    }
    return 0;
}

static void _journal_commit(struct ext4_ctx* ctx) {
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
        _jwrite(ctx, cur, b->b_data);
        cur++;
        if (cur >= j->j_maxlen) cur = j->j_first;
        tag++; tc++; b = b->b_next;
    }

    _jwrite(ctx, start, dbuf);
    kfree_heap(dbuf);

    uint8_t* cbuf = (uint8_t*)kmalloc(ctx->block_size);
    if (!cbuf) return;
    memory_set(cbuf, 0, ctx->block_size);
    hdr = (struct jbd2_header*)cbuf;
    hdr->h_magic     = JBD2_MAGIC_NUMBER;
    hdr->h_blocktype = JBD2_COMMIT_BLOCK;
    hdr->h_sequence  = t->t_tid;
    _jwrite(ctx, cur, cbuf);
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
        _jwrite(ctx, 0, jsb);
        kfree_heap(jsb);
    }
}

static void _journal_start(struct ext4_ctx* ctx) {
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

static void _journal_stop(struct ext4_ctx* ctx) {
    struct jbd2_journal* j = &ctx->journal;
    if (!j->j_sb || !j->j_running_transaction) return;
    _journal_commit(ctx);
    pc_flush_dev(ctx->port, ctx->slave);
    struct jbd2_buffer* b = j->j_running_transaction->t_buffers;
    while (b) { struct jbd2_buffer* nx = b->b_next; kfree_heap(b->b_data); kfree_heap(b); b = nx; }
    kfree_heap(j->j_running_transaction);
    j->j_running_transaction = 0;
}

static void _journal_log(struct ext4_ctx* ctx, uint32_t blocknr, uint8_t* data) {
    struct jbd2_journal* j = &ctx->journal;
    if (!j->j_sb || !j->j_running_transaction) return;
    struct jbd2_buffer* b = j->j_running_transaction->t_buffers;
    while (b) { if (b->b_blocknr == blocknr) { memory_copy(b->b_data, data, ctx->block_size); return; } b = b->b_next; }
    b = (struct jbd2_buffer*)kmalloc(sizeof(*b));
    if (!b) return;
    b->b_blocknr = blocknr;
    b->b_data    = (uint8_t*)kmalloc(ctx->block_size);
    if (!b->b_data) { kfree_heap(b); return; }
    memory_copy(b->b_data, data, ctx->block_size);
    b->b_next = j->j_running_transaction->t_buffers;
    j->j_running_transaction->t_buffers = b;
    j->j_running_transaction->t_nr_buffers++;
}


static void _write_block(struct ext4_ctx* ctx, uint32_t block, uint8_t* buf) {
    _journal_log(ctx, block, buf);

    uint8_t* page = pc_get_page(ctx->port, ctx->slave, block, ctx->block_size);
    if (page) {
        memory_copy(page, buf, ctx->block_size);
        pc_mark_dirty(ctx->port, ctx->slave, block);
        pc_put_page(ctx->port, ctx->slave, block);
    } else {
        uint32_t spb = ctx->block_size / 512;
        uint32_t lba = block * spb;
        for (uint32_t i = 0; i < spb; i++)
            nvme_write_sector(lba + i, buf + i * 512);
    }
}


static void _write_sb(struct ext4_ctx* ctx) {
    uint8_t buf[1024];
    memory_set(buf, 0, sizeof(buf));
    memory_copy(buf, &ctx->sb, sizeof(struct ext4_superblock));
    nvme_write_sector(2, buf);
    nvme_write_sector(3, buf + 512);
}


static uint32_t _gd_base(struct ext4_ctx* ctx) {
    return (ctx->block_size == 1024) ? 2 : 1;
}

static void _read_gd(struct ext4_ctx* ctx, uint32_t group, struct ext4_group_desc* gd) {
    uint32_t ds  = ctx->sb.s_desc_size ? ctx->sb.s_desc_size : 32;
    uint32_t off = group * ds;
    uint8_t* buf = (uint8_t*)kmalloc(ctx->block_size);
    if (!buf) return;
    memory_set(buf, 0, ctx->block_size);
    _read_block(ctx, _gd_base(ctx) + off / ctx->block_size, buf);
    memory_copy(gd, buf + off % ctx->block_size, sizeof(*gd));
    kfree_heap(buf);
}

static void _write_gd(struct ext4_ctx* ctx, uint32_t group, struct ext4_group_desc* gd) {
    uint32_t ds  = ctx->sb.s_desc_size ? ctx->sb.s_desc_size : 32;
    uint32_t off = group * ds;
    uint8_t* buf = (uint8_t*)kmalloc(ctx->block_size);
    if (!buf) return;
    memory_set(buf, 0, ctx->block_size);
    _read_block(ctx, _gd_base(ctx) + off / ctx->block_size, buf);
    memory_copy(buf + off % ctx->block_size, gd, sizeof(*gd));
    _write_block(ctx, _gd_base(ctx) + off / ctx->block_size, buf);
    kfree_heap(buf);
}


static void _read_inode(struct ext4_ctx* ctx, uint32_t ino, struct ext4_inode* out) {
    if (!ino) return;
    struct ext4_group_desc gd;
    uint32_t group = (ino - 1) / ctx->sb.s_inodes_per_group;
    uint32_t index = (ino - 1) % ctx->sb.s_inodes_per_group;
    _read_gd(ctx, group, &gd);
    uint32_t off   = index * ctx->sb.s_inode_size;
    uint8_t* buf   = (uint8_t*)kmalloc(ctx->block_size);
    if (!buf) return;
    memory_set(buf, 0, ctx->block_size);
    _read_block(ctx, gd.bg_inode_table_lo + off / ctx->block_size, buf);
    memory_copy(out, buf + off % ctx->block_size, sizeof(*out));
    kfree_heap(buf);
}

static void _write_inode(struct ext4_ctx* ctx, uint32_t ino, struct ext4_inode* in) {
    if (!ino) return;
    struct ext4_group_desc gd;
    uint32_t group = (ino - 1) / ctx->sb.s_inodes_per_group;
    uint32_t index = (ino - 1) % ctx->sb.s_inodes_per_group;
    _read_gd(ctx, group, &gd);
    uint32_t off   = index * ctx->sb.s_inode_size;
    uint8_t* buf   = (uint8_t*)kmalloc(ctx->block_size);
    if (!buf) return;
    memory_set(buf, 0, ctx->block_size);
    _read_block(ctx, gd.bg_inode_table_lo + off / ctx->block_size, buf);
    memory_copy(buf + off % ctx->block_size, in, sizeof(*in));
    _write_block(ctx, gd.bg_inode_table_lo + off / ctx->block_size, buf);
    kfree_heap(buf);
}


static int  _btest(uint8_t* bm, uint32_t b) { return (bm[b/8] >> (b%8)) & 1; }
static void _bset (uint8_t* bm, uint32_t b) { bm[b/8] |=  (uint8_t)(1 << (b%8)); }
static void _bclr (uint8_t* bm, uint32_t b) { bm[b/8] &= (uint8_t)~(1 << (b%8)); }


static uint32_t _alloc_block_group(struct ext4_ctx* ctx, uint32_t group) {
    struct ext4_group_desc gd;
    _read_gd(ctx, group, &gd);
    if (!gd.bg_free_blocks_count_lo) return 0;
    uint8_t* bm = (uint8_t*)kmalloc(ctx->block_size);
    if (!bm) return 0;
    memory_set(bm, 0, ctx->block_size);
    _read_block(ctx, gd.bg_block_bitmap_lo, bm);
    uint32_t found = 0, ok = 0;
    for (uint32_t i = 0; i < ctx->sb.s_blocks_per_group; i++) {
        if (!_btest(bm, i)) { _bset(bm, i); found = i; ok = 1; break; }
    }
    if (!ok) { kfree_heap(bm); return 0; }
    _write_block(ctx, gd.bg_block_bitmap_lo, bm);
    kfree_heap(bm);
    gd.bg_free_blocks_count_lo--;
    _write_gd(ctx, group, &gd);
    ctx->sb.s_free_blocks_count_lo--;
    _write_sb(ctx);
    return ctx->sb.s_first_data_block + group * ctx->sb.s_blocks_per_group + found;
}

static uint32_t _alloc_block(struct ext4_ctx* ctx) {
    uint32_t groups = (ctx->sb.s_blocks_count_lo + ctx->sb.s_blocks_per_group - 1) / ctx->sb.s_blocks_per_group;
    for (uint32_t g = 0; g < groups; g++) {
        uint32_t b = _alloc_block_group(ctx, g);
        if (b) return b;
    }
    return 0;
}

static void _free_block(struct ext4_ctx* ctx, uint32_t block) {
    if (!block) return;
    uint32_t rel   = block - ctx->sb.s_first_data_block;
    uint32_t group = rel / ctx->sb.s_blocks_per_group;
    uint32_t index = rel % ctx->sb.s_blocks_per_group;
    struct ext4_group_desc gd;
    _read_gd(ctx, group, &gd);
    uint8_t* bm = (uint8_t*)kmalloc(ctx->block_size);
    if (!bm) return;
    memory_set(bm, 0, ctx->block_size);
    _read_block(ctx, gd.bg_block_bitmap_lo, bm);
    _bclr(bm, index);
    _write_block(ctx, gd.bg_block_bitmap_lo, bm);
    kfree_heap(bm);
    gd.bg_free_blocks_count_lo++;
    _write_gd(ctx, group, &gd);
    ctx->sb.s_free_blocks_count_lo++;
    _write_sb(ctx);
}


static uint32_t _alloc_inode_group(struct ext4_ctx* ctx, uint32_t group) {
    struct ext4_group_desc gd;
    _read_gd(ctx, group, &gd);
    if (!gd.bg_free_inodes_count_lo) return 0;
    uint8_t* bm = (uint8_t*)kmalloc(ctx->block_size);
    if (!bm) return 0;
    memory_set(bm, 0, ctx->block_size);
    _read_block(ctx, gd.bg_inode_bitmap_lo, bm);
    uint32_t found = 0, ok = 0;
    for (uint32_t i = 0; i < ctx->sb.s_inodes_per_group; i++) {
        if (!_btest(bm, i)) { _bset(bm, i); found = i; ok = 1; break; }
    }
    if (!ok) { kfree_heap(bm); return 0; }
    _write_block(ctx, gd.bg_inode_bitmap_lo, bm);
    kfree_heap(bm);
    gd.bg_free_inodes_count_lo--;
    _write_gd(ctx, group, &gd);
    ctx->sb.s_free_inodes_count--;
    _write_sb(ctx);
    return group * ctx->sb.s_inodes_per_group + found + 1;
}

static uint32_t _alloc_inode(struct ext4_ctx* ctx) {
    uint32_t groups = (ctx->sb.s_blocks_count_lo + ctx->sb.s_blocks_per_group - 1) / ctx->sb.s_blocks_per_group;
    for (uint32_t g = 0; g < groups; g++) {
        uint32_t ino = _alloc_inode_group(ctx, g);
        if (ino) return ino;
    }
    return 0;
}

static void _free_inode(struct ext4_ctx* ctx, uint32_t ino) {
    if (!ino) return;
    uint32_t group = (ino - 1) / ctx->sb.s_inodes_per_group;
    uint32_t index = (ino - 1) % ctx->sb.s_inodes_per_group;
    struct ext4_group_desc gd;
    _read_gd(ctx, group, &gd);
    uint8_t* bm = (uint8_t*)kmalloc(ctx->block_size);
    if (!bm) return;
    memory_set(bm, 0, ctx->block_size);
    _read_block(ctx, gd.bg_inode_bitmap_lo, bm);
    _bclr(bm, index);
    _write_block(ctx, gd.bg_inode_bitmap_lo, bm);
    kfree_heap(bm);
    gd.bg_free_inodes_count_lo++;
    _write_gd(ctx, group, &gd);
    ctx->sb.s_free_inodes_count++;
    _write_sb(ctx);
}


static void _extent_init(struct ext4_inode* inode) {
    memory_set(inode->i_block, 0, sizeof(inode->i_block));
    struct ext4_extent_header* eh = (struct ext4_extent_header*)inode->i_block;
    eh->eh_magic   = 0xF30A;
    eh->eh_entries = 0;
    eh->eh_max     = 4;
    eh->eh_depth   = 0;
}

static uint32_t _extent_pblock(struct ext4_inode* inode, uint32_t fb) {
    struct ext4_extent_header* eh = (struct ext4_extent_header*)inode->i_block;
    if (eh->eh_magic != 0xF30A || eh->eh_depth != 0) return 0;
    struct ext4_extent* ee = (struct ext4_extent*)((uint8_t*)inode->i_block + sizeof(*eh));
    for (uint16_t i = 0; i < eh->eh_entries; i++)
        if (fb >= ee[i].ee_block && fb < ee[i].ee_block + ee[i].ee_len)
            return ee[i].ee_start_lo + (fb - ee[i].ee_block);
    return 0;
}

static int _extent_add(struct ext4_inode* inode, uint32_t fb, uint32_t pb, uint16_t len) {
    struct ext4_extent_header* eh = (struct ext4_extent_header*)inode->i_block;
    if (eh->eh_entries >= eh->eh_max) return -1;
    struct ext4_extent* ee = (struct ext4_extent*)((uint8_t*)inode->i_block + sizeof(*eh));
    uint16_t idx = eh->eh_entries;
    ee[idx].ee_block    = fb;
    ee[idx].ee_len      = len;
    ee[idx].ee_start_hi = 0;
    ee[idx].ee_start_lo = pb;
    eh->eh_entries++;
    return 0;
}


static void _iter_dir(struct ext4_ctx* ctx, vfs_node_t* node,
                      void (*cb)(struct ext4_dir_entry_2*, void*), void* ud) {
    struct ext4_inode inode;
    _read_inode(ctx, node->inode, &inode);
    uint8_t* buf = (uint8_t*)kmalloc(ctx->block_size);
    if (!buf) return;

    struct ext4_extent_header* eh = (struct ext4_extent_header*)inode.i_block;
    if (eh->eh_magic == 0xF30A) {
        struct ext4_extent* ee = (struct ext4_extent*)((uint8_t*)inode.i_block + sizeof(*eh));
        for (uint16_t ei = 0; ei < eh->eh_entries; ei++) {
            for (uint32_t bi = 0; bi < ee[ei].ee_len; bi++) {
                memory_set(buf, 0, ctx->block_size);
                _read_block(ctx, ee[ei].ee_start_lo + bi, buf);
                uint32_t off = 0;
                while (off + sizeof(struct ext4_dir_entry_2) <= ctx->block_size) {
                    struct ext4_dir_entry_2* de = (struct ext4_dir_entry_2*)(buf + off);
                    if (!de->rec_len) break;
                    if (de->inode && de->name_len) cb(de, ud);
                    off += de->rec_len;
                }
            }
        }
    } else {
        for (int bi = 0; bi < 12; bi++) {
            if (!inode.i_block[bi]) break;
            memory_set(buf, 0, ctx->block_size);
            _read_block(ctx, inode.i_block[bi], buf);
            uint32_t off = 0;
            while (off + sizeof(struct ext4_dir_entry_2) <= ctx->block_size) {
                struct ext4_dir_entry_2* de = (struct ext4_dir_entry_2*)(buf + off);
                if (!de->rec_len) break;
                if (de->inode && de->name_len) cb(de, ud);
                off += de->rec_len;
            }
        }
    }
    kfree_heap(buf);
}


static int _dir_add(struct ext4_ctx* ctx, vfs_node_t* node,
                    uint32_t entry_ino, const char* name, uint8_t ft) {
    struct ext4_inode di;
    _read_inode(ctx, node->inode, &di);
    struct ext4_extent_header* eh = (struct ext4_extent_header*)di.i_block;
    if (eh->eh_magic != 0xF30A) { _extent_init(&di); eh = (struct ext4_extent_header*)di.i_block; }

    uint8_t  nl     = 0; while (name[nl]) nl++;
    uint16_t needed = (uint16_t)((sizeof(struct ext4_dir_entry_2) + nl + 3) & ~3u);
    uint8_t* buf    = (uint8_t*)kmalloc(ctx->block_size);
    if (!buf) return -1;

    struct ext4_extent* ee = (struct ext4_extent*)((uint8_t*)di.i_block + sizeof(*eh));
    for (uint16_t ei = 0; ei < eh->eh_entries; ei++) {
        for (uint32_t bi = 0; bi < ee[ei].ee_len; bi++) {
            uint32_t phys = ee[ei].ee_start_lo + bi;
            memory_set(buf, 0, ctx->block_size);
            _read_block(ctx, phys, buf);
            uint32_t off = 0;
            while (off + sizeof(struct ext4_dir_entry_2) <= ctx->block_size) {
                struct ext4_dir_entry_2* de = (struct ext4_dir_entry_2*)(buf + off);
                if (!de->rec_len) break;
                uint16_t real = de->inode
                    ? (uint16_t)((sizeof(struct ext4_dir_entry_2) + de->name_len + 3) & ~3u) : 0;
                if (!de->inode && de->rec_len >= needed) {
                    de->inode = entry_ino; de->name_len = nl; de->file_type = ft;
                    memory_copy(de->name, name, nl);
                    _write_block(ctx, phys, buf); kfree_heap(buf); return 0;
                }
                if (de->inode && (de->rec_len - real) >= needed) {
                    uint16_t old = de->rec_len; de->rec_len = real;
                    struct ext4_dir_entry_2* nd = (struct ext4_dir_entry_2*)(buf + off + real);
                    nd->inode = entry_ino; nd->rec_len = old - real;
                    nd->name_len = nl; nd->file_type = ft;
                    memory_copy(nd->name, name, nl);
                    _write_block(ctx, phys, buf); kfree_heap(buf); return 0;
                }
                off += de->rec_len;
            }
        }
    }

    uint32_t np = _alloc_block(ctx);
    if (!np) { kfree_heap(buf); return -1; }
    memory_set(buf, 0, ctx->block_size);
    struct ext4_dir_entry_2* de = (struct ext4_dir_entry_2*)buf;
    de->inode = entry_ino; de->rec_len = (uint16_t)ctx->block_size;
    de->name_len = nl; de->file_type = ft;
    memory_copy(de->name, name, nl);
    _write_block(ctx, np, buf);
    kfree_heap(buf);

    uint32_t next_fb = 0;
    for (uint16_t ei = 0; ei < eh->eh_entries; ei++) next_fb += ee[ei].ee_len;
    if (_extent_add(&di, next_fb, np, 1) < 0) { _free_block(ctx, np); return -1; }
    di.i_size_lo   += ctx->block_size;
    di.i_blocks_lo += ctx->block_size / 512;
    _write_inode(ctx, node->inode, &di);
    return 0;
}

static int _dir_remove(struct ext4_ctx* ctx, vfs_node_t* node,
                       const char* name, uint32_t* out_ino, uint8_t* out_ft) {
    struct ext4_inode di;
    _read_inode(ctx, node->inode, &di);
    struct ext4_extent_header* eh = (struct ext4_extent_header*)di.i_block;
    if (eh->eh_magic != 0xF30A) return -1;
    uint8_t nl = 0; while (name[nl]) nl++;
    uint8_t* buf = (uint8_t*)kmalloc(ctx->block_size);
    if (!buf) return -1;
    struct ext4_extent* ee = (struct ext4_extent*)((uint8_t*)di.i_block + sizeof(*eh));
    for (uint16_t ei = 0; ei < eh->eh_entries; ei++) {
        for (uint32_t bi = 0; bi < ee[ei].ee_len; bi++) {
            uint32_t phys = ee[ei].ee_start_lo + bi;
            memory_set(buf, 0, ctx->block_size);
            _read_block(ctx, phys, buf);
            uint32_t off = 0;
            while (off + sizeof(struct ext4_dir_entry_2) <= ctx->block_size) {
                struct ext4_dir_entry_2* de = (struct ext4_dir_entry_2*)(buf + off);
                if (!de->rec_len) break;
                if (de->inode && de->name_len == nl) {
                    char en[256]; memory_copy(en, de->name, nl); en[nl] = '\0';
                    if (compare_string(en, name) == 0) {
                        if (out_ino) *out_ino = de->inode;
                        if (out_ft)  *out_ft  = de->file_type;
                        de->inode = 0;
                        _write_block(ctx, phys, buf); kfree_heap(buf); return 0;
                    }
                }
                off += de->rec_len;
            }
        }
    }
    kfree_heap(buf);
    return -1;
}

static int _dir_empty(struct ext4_ctx* ctx, uint32_t ino) {
    struct ext4_inode di;
    _read_inode(ctx, ino, &di);
    struct ext4_extent_header* eh = (struct ext4_extent_header*)di.i_block;
    if (eh->eh_magic != 0xF30A) return 1;
    uint8_t* buf = (uint8_t*)kmalloc(ctx->block_size);
    if (!buf) return 1;
    struct ext4_extent* ee = (struct ext4_extent*)((uint8_t*)di.i_block + sizeof(*eh));
    for (uint16_t ei = 0; ei < eh->eh_entries; ei++) {
        for (uint32_t bi = 0; bi < ee[ei].ee_len; bi++) {
            memory_set(buf, 0, ctx->block_size);
            _read_block(ctx, ee[ei].ee_start_lo + bi, buf);
            uint32_t off = 0;
            while (off + sizeof(struct ext4_dir_entry_2) <= ctx->block_size) {
                struct ext4_dir_entry_2* de = (struct ext4_dir_entry_2*)(buf + off);
                if (!de->rec_len) break;
                if (de->inode && de->name_len) {
                    char en[256]; memory_copy(en, de->name, de->name_len); en[de->name_len] = '\0';
                    if (compare_string(en, ".") != 0 && compare_string(en, "..") != 0) {
                        kfree_heap(buf); return 0;
                    }
                }
                off += de->rec_len;
            }
        }
    }
    kfree_heap(buf);
    return 1;
}


typedef struct { uint32_t target_idx; uint32_t cur; vfs_dirent_t de; int found; } _rdctx_t;

static void _readdir_cb(struct ext4_dir_entry_2* de, void* ud) {
    _rdctx_t* rc = (_rdctx_t*)ud;
    if (rc->found) return;
    char name[256];
    memory_copy(name, de->name, de->name_len);
    name[de->name_len] = '\0';
    if (name[0] == '.' && (name[1] == '\0' || (name[1] == '.' && name[2] == '\0'))) return;
    if (rc->cur++ == rc->target_idx) {
        memory_copy(rc->de.name, name, de->name_len + 1);
        rc->de.inode = de->inode;
        rc->found = 1;
    }
}

static vfs_dirent_t* ext4_readdir(vfs_node_t* node, uint32_t index) {
    static _rdctx_t rc;
    rc.target_idx = index;
    rc.cur        = 0;
    rc.found      = 0;
    memory_set(rc.de.name, 0, 128);
    struct ext4_ctx* ctx = (struct ext4_ctx*)node->priv;
    _iter_dir(ctx, node, _readdir_cb, &rc);
    return rc.found ? &rc.de : 0;
}


static vfs_node_t* _ops_walk  (vfs_node_t *d, const char *n) { return ext4_finddir(d, (char*)n); }
static int         _ops_create(vfs_node_t *d, const char *n) { return ext4_create (d, (char*)n); }
static int         _ops_delete(vfs_node_t *d, const char *n) { return ext4_delete (d, (char*)n); }
static int         _ops_mkdir (vfs_node_t *d, const char *n) { return ext4_mkdir  (d, (char*)n); }
static int         _ops_rmdir (vfs_node_t *d, const char *n) { return ext4_rmdir  (d, (char*)n); }


static vfs_ops_t ext4_dir_ops = {
    .walk    = _ops_walk,
    .readdir = ext4_readdir,
    .listdir = ext4_list_dir,
    .mkdir   = _ops_mkdir,
    .rmdir   = _ops_rmdir,
    .create  = _ops_create,
    .delete  = _ops_delete,
};

static vfs_ops_t ext4_file_ops = {
    .read   = ext4_read_file,
    .write  = ext4_write_file,
    .delete = _ops_delete,
};

static void _make_node(struct ext4_ctx* ctx, vfs_node_t* n, uint32_t ino, uint8_t ft) {
    memory_set(n, 0, sizeof(*n));
    n->inode = ino;
    n->priv  = ctx;  
    if (ft == EXT4_FT_DIR) {
        n->type = VFS_DIRECTORY;
        n->ops  = &ext4_dir_ops;
    } else {
        n->type = VFS_FILE;
        n->ops  = &ext4_file_ops;
        struct ext4_inode fi;
        _read_inode(ctx, ino, &fi);
        n->size = fi.i_size_lo;
    }
}


static void _listdir_cb(struct ext4_dir_entry_2* de, void* ud) {
    (void)ud;
    char name[256];
    memory_copy(name, de->name, de->name_len);
    name[de->name_len] = '\0';
    if (de->file_type == EXT4_FT_DIR) {
        kprint_color(name, COLOR_LIGHT_CYAN);
        kprint("/\n");
    } else {
        kprint(name);
        kprint("\n");
    }
}


typedef struct { char* target; vfs_node_t* result; struct ext4_ctx* ctx; } _fctx_t;

static void _finddir_cb(struct ext4_dir_entry_2* de, void* ud) {
    _fctx_t* fc = (_fctx_t*)ud;
    if (fc->result) return;
    char name[256];
    memory_copy(name, de->name, de->name_len);
    name[de->name_len] = '\0';
    if (compare_string(name, fc->target) != 0) return;
    vfs_node_t* res = (vfs_node_t*)kmalloc(sizeof(*res));
    if (!res) return;
    copy_string(res->name, name);
    _make_node(fc->ctx, res, de->inode, de->file_type);
    fc->result = res;
}

//Public api
void ext4_list_dir(vfs_node_t* node) {
    struct ext4_ctx* ctx = (struct ext4_ctx*)node->priv;
    _iter_dir(ctx, node, _listdir_cb, 0);
}

vfs_node_t* ext4_finddir(vfs_node_t* node, char* name) {
    struct ext4_ctx* ctx = (struct ext4_ctx*)node->priv;
    _fctx_t fc; fc.target = name; fc.result = 0; fc.ctx = ctx;
    _iter_dir(ctx, node, _finddir_cb, &fc);
    return fc.result;
}

int ext4_read_file(vfs_node_t* node, uint32_t offset, uint32_t size, char* buffer) {
    struct ext4_ctx* ctx = (struct ext4_ctx*)node->priv;
    struct ext4_inode inode;
    _read_inode(ctx, node->inode, &inode);
    uint32_t fsz = inode.i_size_lo;
    if (offset >= fsz) return 0;
    if (offset + size > fsz) size = fsz - offset;
    if (!size) return 0;
    uint8_t* tmp = (uint8_t*)kmalloc(ctx->block_size);
    if (!tmp) return -1;
    struct ext4_extent_header* eh = (struct ext4_extent_header*)inode.i_block;
    int use_ext = (eh->eh_magic == 0xF30A);
    uint32_t read = 0, cur = offset;
    while (read < size) {
        uint32_t fb = cur / ctx->block_size;
        uint32_t ibo = cur % ctx->block_size;
        uint32_t pb = use_ext ? _extent_pblock(&inode, fb) : (fb < 12 ? inode.i_block[fb] : 0);
        if (!pb) break;
        memory_set(tmp, 0, ctx->block_size);
        _read_block(ctx, pb, tmp);
        uint32_t tc = ctx->block_size - ibo;
        if (tc > size - read) tc = size - read;
        memory_copy(buffer + read, tmp + ibo, tc);
        read += tc; cur += tc;
    }
    kfree_heap(tmp);
    return (int)read;
}

int ext4_write_file(vfs_node_t* node, uint32_t offset, uint32_t size, char* buffer) {
    if (!size) return 0;
    struct ext4_ctx* ctx = (struct ext4_ctx*)node->priv;
    _journal_start(ctx);
    struct ext4_inode inode;
    _read_inode(ctx, node->inode, &inode);
    struct ext4_extent_header* eh = (struct ext4_extent_header*)inode.i_block;
    if (eh->eh_magic != 0xF30A) _extent_init(&inode);
    uint8_t* tmp = (uint8_t*)kmalloc(ctx->block_size);
    if (!tmp) { _journal_stop(ctx); return -1; }
    uint32_t written = 0, cur = offset;
    while (written < size) {
        uint32_t fb  = cur / ctx->block_size;
        uint32_t ibo = cur % ctx->block_size;
        uint32_t pb  = _extent_pblock(&inode, fb);
        if (!pb) {
            pb = _alloc_block(ctx);
            if (!pb) break;
            memory_set(tmp, 0, ctx->block_size);
            _write_block(ctx, pb, tmp);
            if (_extent_add(&inode, fb, pb, 1) < 0) { _free_block(ctx, pb); break; }
            inode.i_blocks_lo += ctx->block_size / 512;
        }
        memory_set(tmp, 0, ctx->block_size);
        _read_block(ctx, pb, tmp);
        uint32_t tc = ctx->block_size - ibo;
        if (tc > size - written) tc = size - written;
        memory_copy(tmp + ibo, buffer + written, tc);
        _write_block(ctx, pb, tmp);
        written += tc; cur += tc;
    }
    kfree_heap(tmp);
    if (cur > inode.i_size_lo) { inode.i_size_lo = cur; node->size = cur; }
    _write_inode(ctx, node->inode, &inode);
    _journal_stop(ctx);
    return (int)written;
}

int ext4_create(vfs_node_t* node, char* name) {
    struct ext4_ctx* ctx = (struct ext4_ctx*)node->priv;
    _journal_start(ctx);
    vfs_node_t* ex = ext4_finddir(node, name);
    if (ex) { kfree_heap(ex); _journal_stop(ctx); return -1; }
    uint32_t ino = _alloc_inode(ctx);
    if (!ino) { _journal_stop(ctx); return -1; }
    struct ext4_inode ni;
    memory_set(&ni, 0, sizeof(ni));
    ni.i_mode = 0x81A4; ni.i_links_count = 1; ni.i_flags = 0x80000;
    _extent_init(&ni);
    _write_inode(ctx, ino, &ni);
    if (_dir_add(ctx, node, ino, name, EXT4_FT_REG_FILE) < 0) {
        _free_inode(ctx, ino); _journal_stop(ctx); return -1;
    }
    _journal_stop(ctx);
    return 0;
}

int ext4_mkdir(vfs_node_t* node, char* name) {
    struct ext4_ctx* ctx = (struct ext4_ctx*)node->priv;
    _journal_start(ctx);
    vfs_node_t* ex = ext4_finddir(node, name);
    if (ex) { kfree_heap(ex); _journal_stop(ctx); return -1; }
    uint32_t ino = _alloc_inode(ctx);
    if (!ino) { _journal_stop(ctx); return -1; }
    uint32_t db = _alloc_block(ctx);
    if (!db) { _free_inode(ctx, ino); _journal_stop(ctx); return -1; }

    struct ext4_inode ni;
    memory_set(&ni, 0, sizeof(ni));
    ni.i_mode = 0x41ED; ni.i_links_count = 2;
    ni.i_size_lo = ctx->block_size; ni.i_blocks_lo = ctx->block_size / 512;
    ni.i_flags = 0x80000;
    _extent_init(&ni); _extent_add(&ni, 0, db, 1);
    _write_inode(ctx, ino, &ni);

    uint8_t* buf = (uint8_t*)kmalloc(ctx->block_size);
    if (!buf) { _free_inode(ctx, ino); _free_block(ctx, db); _journal_stop(ctx); return -1; }
    memory_set(buf, 0, ctx->block_size);
    struct ext4_dir_entry_2* dot = (struct ext4_dir_entry_2*)buf;
    dot->inode = ino; dot->rec_len = 12; dot->name_len = 1;
    dot->file_type = EXT4_FT_DIR; dot->name[0] = '.';
    struct ext4_dir_entry_2* dd = (struct ext4_dir_entry_2*)(buf + 12);
    dd->inode = node->inode; dd->rec_len = (uint16_t)(ctx->block_size - 12);
    dd->name_len = 2; dd->file_type = EXT4_FT_DIR;
    dd->name[0] = '.'; dd->name[1] = '.';
    _write_block(ctx, db, buf);
    kfree_heap(buf);

    struct ext4_inode pi; _read_inode(ctx, node->inode, &pi);
    pi.i_links_count++; _write_inode(ctx, node->inode, &pi);

    if (_dir_add(ctx, node, ino, name, EXT4_FT_DIR) < 0) {
        _free_inode(ctx, ino); _free_block(ctx, db); _journal_stop(ctx); return -1;
    }
    _journal_stop(ctx);
    return 0;
}

int ext4_delete(vfs_node_t* node, char* name) {
    struct ext4_ctx* ctx = (struct ext4_ctx*)node->priv;
    _journal_start(ctx);
    uint32_t del_ino = 0; uint8_t del_ft = 0;
    if (_dir_remove(ctx, node, name, &del_ino, &del_ft) < 0 || !del_ino) {
        _journal_stop(ctx); return -1;
    }
    if (del_ft == EXT4_FT_DIR) { _journal_stop(ctx); return -1; }
    struct ext4_inode inode; _read_inode(ctx, del_ino, &inode);
    if (inode.i_links_count) inode.i_links_count--;
    if (!inode.i_links_count) {
        struct ext4_extent_header* eh = (struct ext4_extent_header*)inode.i_block;
        if (eh->eh_magic == 0xF30A) {
            struct ext4_extent* ee = (struct ext4_extent*)((uint8_t*)inode.i_block + sizeof(*eh));
            for (uint16_t i = 0; i < eh->eh_entries; i++)
                for (uint32_t b = 0; b < ee[i].ee_len; b++)
                    _free_block(ctx, ee[i].ee_start_lo + b);
        }
        inode.i_dtime = 1; _write_inode(ctx, del_ino, &inode); _free_inode(ctx, del_ino);
    } else {
        _write_inode(ctx, del_ino, &inode);
    }
    _journal_stop(ctx);
    return 0;
}

int ext4_rmdir(vfs_node_t* node, char* name) {
    struct ext4_ctx* ctx = (struct ext4_ctx*)node->priv;
    _journal_start(ctx);
    vfs_node_t* tgt = ext4_finddir(node, name);
    if (!tgt) { _journal_stop(ctx); return -1; }
    if (tgt->type != VFS_DIRECTORY) { kfree_heap(tgt); _journal_stop(ctx); return -1; }
    uint32_t tino = tgt->inode; kfree_heap(tgt);
    if (!_dir_empty(ctx, tino)) { _journal_stop(ctx); return -1; }
    uint32_t del_ino = 0; uint8_t del_ft = 0;
    if (_dir_remove(ctx, node, name, &del_ino, &del_ft) < 0) { _journal_stop(ctx); return -1; }
    struct ext4_inode di; _read_inode(ctx, del_ino, &di);
    struct ext4_extent_header* eh = (struct ext4_extent_header*)di.i_block;
    if (eh->eh_magic == 0xF30A) {
        struct ext4_extent* ee = (struct ext4_extent*)((uint8_t*)di.i_block + sizeof(*eh));
        for (uint16_t i = 0; i < eh->eh_entries; i++)
            for (uint32_t b = 0; b < ee[i].ee_len; b++)
                _free_block(ctx, ee[i].ee_start_lo + b);
    }
    di.i_dtime = 1; _write_inode(ctx, del_ino, &di); _free_inode(ctx, del_ino);
    struct ext4_inode pi; _read_inode(ctx, node->inode, &pi);
    if (pi.i_links_count > 1) pi.i_links_count--;
    _write_inode(ctx, node->inode, &pi);
    _journal_stop(ctx);
    return 0;
}


vfs_node_t* ext4_mount_disk(uint16_t port, uint8_t slave) {
    struct ext4_ctx* ctx = (struct ext4_ctx*)kmalloc(sizeof(*ctx));
    if (!ctx) return 0;
    memory_set(ctx, 0, sizeof(*ctx));
    ctx->port  = port;
    ctx->slave = slave;

    uint8_t buf[2048];
    memory_set(buf, 0, sizeof(buf));
    nvme_read_sector(2, buf);
    nvme_read_sector(3, buf + 512);
    memory_copy(&ctx->sb, buf, 1024);

    if (ctx->sb.s_magic != EXT4_SUPER_MAGIC) {
        nvme_read_sector(0, buf);
        nvme_read_sector(1, buf + 512);
        memory_copy(&ctx->sb, buf, 1024);
    }

    if (ctx->sb.s_magic != EXT4_SUPER_MAGIC) {
        kprint("[ext4] ERROR: superblock not found\n");
        kfree_heap(ctx);
        return 0;
    }

    ctx->block_size = 1024u << ctx->sb.s_log_block_size;

    vfs_node_t* root = (vfs_node_t*)kmalloc(sizeof(*root));
    if (!root) { kfree_heap(ctx); return 0; }
    memory_set(root, 0, sizeof(*root));
    copy_string(root->name, "/");
    root->inode = 2;
    _make_node(ctx, root, 2, EXT4_FT_DIR);
    copy_string(root->name, "/");

    if (ctx->sb.s_feature_compat & 0x0004) {
        ctx->journal.j_inum = ctx->sb.s_journal_inum;
        if (ctx->journal.j_inum) {
            uint8_t* jsb = (uint8_t*)kmalloc(ctx->block_size);
            if (jsb) {
                memory_set(jsb, 0, ctx->block_size);
                _jread(ctx, 0, jsb);
                ctx->journal.j_sb = (struct jbd2_superblock*)jsb;
                uint32_t m = ctx->journal.j_sb->s_header.h_magic;
                uint32_t mb = ((m&0xff000000)>>24)|((m&0x00ff0000)>>8)|
                              ((m&0x0000ff00)<<8)|((m&0x000000ff)<<24);
                if (m == JBD2_MAGIC_NUMBER || mb == JBD2_MAGIC_NUMBER) {
                    ctx->journal.j_maxlen = ctx->journal.j_sb->s_maxlen;
                    ctx->journal.j_first  = ctx->journal.j_sb->s_first;
                    ctx->journal.j_head   = ctx->journal.j_sb->s_start;
                    ctx->journal.j_tail   = ctx->journal.j_sb->s_start;
                    ctx->journal.j_running_transaction = 0;
                } else {
                    kprint("[ext4] WARNING: invalid journal magic\n");
                    kfree_heap(jsb);
                    ctx->journal.j_sb = 0;
                }
            }
        }
    } else {
        kprint("[ext4] WARNING: Filesystem mounted without journaling.\n");
        ctx->journal.j_sb = 0;
        ctx->journal.j_running_transaction = 0;
    }

    return root;
}

void ext4_init(void) {
    vfs_root = ext4_mount_disk(0, 0);
}