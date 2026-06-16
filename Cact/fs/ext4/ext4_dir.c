#include "ext4_internal.h"
#include "klib.h"
#include "memory.h"

void ext4_dir_iter(struct ext4_ctx* ctx, vfs_node_t* node,
                     void (*cb)(struct ext4_dir_entry_2*, void*), void* ud) {
    if (!cb) return;
    struct ext4_inode inode;
    ext4_read_inode(ctx, node->inode, &inode);
    uint8_t* buf = (uint8_t*)kmalloc(ctx->block_size);
    if (!buf) return;

    struct ext4_extent_header* eh = (struct ext4_extent_header*)inode.i_block;
    if (eh->eh_magic == 0xF30A) {
        uint16_t nr = eh->eh_entries;
        if (nr > EXT4_MAX_EXTENTS) nr = EXT4_MAX_EXTENTS;
        struct ext4_extent* ee = (struct ext4_extent*)((uint8_t*)inode.i_block + sizeof(*eh));
        for (uint16_t ei = 0; ei < nr; ei++) {
            uint16_t elen = ee[ei].ee_len;
            if (elen > 32768) elen = 32768;
            for (uint32_t bi = 0; bi < elen; bi++) {
                memory_set(buf, 0, ctx->block_size);
                ext4_read_block(ctx, ee[ei].ee_start_lo + bi, buf);
                uint32_t off = 0;
                while (off + sizeof(struct ext4_dir_entry_2) <= ctx->block_size) {
                    struct ext4_dir_entry_2* de = (struct ext4_dir_entry_2*)(buf + off);
                    if (!de->rec_len || de->rec_len < sizeof(struct ext4_dir_entry_2)) break;
                    if (off + de->rec_len > ctx->block_size) break;
                    if (de->inode && de->name_len) cb(de, ud);
                    off += de->rec_len;
                }
            }
        }
    } else {
        uint32_t total_blocks = inode.i_size_lo / ctx->block_size;
        if (inode.i_size_lo % ctx->block_size) total_blocks++;
        for (uint32_t bi = 0; bi < total_blocks; bi++) {
            uint32_t pb = ext4_legacy_bmap(ctx, &inode, bi);
            if (!pb) break;
            memory_set(buf, 0, ctx->block_size);
            ext4_read_block(ctx, pb, buf);
            uint32_t off = 0;
            while (off + sizeof(struct ext4_dir_entry_2) <= ctx->block_size) {
                struct ext4_dir_entry_2* de = (struct ext4_dir_entry_2*)(buf + off);
                if (!de->rec_len || de->rec_len < sizeof(struct ext4_dir_entry_2)) break;
                if (off + de->rec_len > ctx->block_size) break;
                if (de->inode && de->name_len) cb(de, ud);
                off += de->rec_len;
            }
        }
    }
    kfree_heap(buf);
}

int ext4_dir_add(struct ext4_ctx* ctx, vfs_node_t* node, uint32_t entry_ino, const char* name,
                 uint8_t ft) {
    struct ext4_inode di;
    ext4_read_inode(ctx, node->inode, &di);
    struct ext4_extent_header* eh = (struct ext4_extent_header*)di.i_block;
    if (eh->eh_magic != 0xF30A) {
        ext4_extent_init(&di);
        eh = (struct ext4_extent_header*)di.i_block;
    }

    uint8_t nl = 0;
    while (name[nl]) nl++;
    uint16_t needed = (uint16_t)((sizeof(struct ext4_dir_entry_2) + nl + 3) & ~3u);
    uint8_t* buf    = (uint8_t*)kmalloc(ctx->block_size);
    if (!buf) return -1;

    struct ext4_extent* ee = (struct ext4_extent*)((uint8_t*)di.i_block + sizeof(*eh));
    uint16_t nr = eh->eh_entries;
    if (nr > EXT4_MAX_EXTENTS) nr = EXT4_MAX_EXTENTS;
    for (uint16_t ei = 0; ei < nr; ei++) {
        uint16_t elen = ee[ei].ee_len;
        if (elen > 32768) elen = 32768;
        for (uint32_t bi = 0; bi < elen; bi++) {
            uint32_t phys = ee[ei].ee_start_lo + bi;
            memory_set(buf, 0, ctx->block_size);
            ext4_read_block(ctx, phys, buf);
            uint32_t off = 0;
            while (off + sizeof(struct ext4_dir_entry_2) <= ctx->block_size) {
                struct ext4_dir_entry_2* de = (struct ext4_dir_entry_2*)(buf + off);
                if (!de->rec_len || de->rec_len < sizeof(struct ext4_dir_entry_2)) break;
                if (off + de->rec_len > ctx->block_size) break;
                uint16_t real = de->inode
                    ? (uint16_t)((sizeof(struct ext4_dir_entry_2) + de->name_len + 3) & ~3u)
                    : 0;
                if (!de->inode && de->rec_len >= needed) {
                    de->inode = entry_ino;
                    de->name_len = nl;
                    de->file_type = ft;
                    memory_copy(de->name, name, nl);
                    ext4_write_block(ctx, phys, buf);
                    kfree_heap(buf);
                    return 0;
                }
                if (de->inode && (de->rec_len - real) >= needed) {
                    uint16_t old = de->rec_len;
                    de->rec_len    = real;
                    struct ext4_dir_entry_2* nd = (struct ext4_dir_entry_2*)(buf + off + real);
                    nd->inode    = entry_ino;
                    nd->rec_len  = old - real;
                    nd->name_len = nl;
                    nd->file_type = ft;
                    memory_copy(nd->name, name, nl);
                    ext4_write_block(ctx, phys, buf);
                    kfree_heap(buf);
                    return 0;
                }
                off += de->rec_len;
            }
        }
    }

    uint32_t np = ext4_alloc_block(ctx);
    if (!np) { kfree_heap(buf); return -1; }
    memory_set(buf, 0, ctx->block_size);
    struct ext4_dir_entry_2* de = (struct ext4_dir_entry_2*)buf;
    de->inode    = entry_ino;
    de->rec_len  = (uint16_t)ctx->block_size;
    de->name_len = nl;
    de->file_type = ft;
    memory_copy(de->name, name, nl);
    ext4_write_block(ctx, np, buf);
    kfree_heap(buf);

    uint32_t next_fb = 0;
    for (uint16_t ei = 0; ei < nr; ei++) next_fb += ee[ei].ee_len;
    if (ext4_extent_add(&di, next_fb, np, 1) < 0) {
        ext4_free_block(ctx, np);
        return -1;
    }
    di.i_size_lo += ctx->block_size;
    di.i_blocks_lo += ctx->block_size / 512;
    ext4_write_inode(ctx, node->inode, &di);
    return 0;
}

int ext4_dir_remove(struct ext4_ctx* ctx, vfs_node_t* node, const char* name, uint32_t* out_ino,
                    uint8_t* out_ft) {
    struct ext4_inode di;
    ext4_read_inode(ctx, node->inode, &di);
    struct ext4_extent_header* eh = (struct ext4_extent_header*)di.i_block;
    if (eh->eh_magic != 0xF30A) return -1;

    int nl = 0;
    while (name[nl]) nl++;
    if (nl > 255) return -1;
    uint8_t* buf = (uint8_t*)kmalloc(ctx->block_size);
    if (!buf) return -1;

    struct ext4_extent* ee = (struct ext4_extent*)((uint8_t*)di.i_block + sizeof(*eh));
    uint16_t nr = eh->eh_entries;
    if (nr > EXT4_MAX_EXTENTS) nr = EXT4_MAX_EXTENTS;
    for (uint16_t ei = 0; ei < nr; ei++) {
        uint16_t elen = ee[ei].ee_len;
        if (elen > 32768) elen = 32768;
        for (uint32_t bi = 0; bi < elen; bi++) {
            uint32_t phys = ee[ei].ee_start_lo + bi;
            memory_set(buf, 0, ctx->block_size);
            ext4_read_block(ctx, phys, buf);
            uint32_t off = 0;
            while (off + sizeof(struct ext4_dir_entry_2) <= ctx->block_size) {
                struct ext4_dir_entry_2* de = (struct ext4_dir_entry_2*)(buf + off);
                if (!de->rec_len || de->rec_len < sizeof(struct ext4_dir_entry_2)) break;
                if (off + de->rec_len > ctx->block_size) break;
                if (de->inode && de->name_len == (uint8_t)nl) {
                    char en[256];
                    memory_copy(en, de->name, nl);
                    en[nl] = '\0';
                    if (compare_string(en, name) == 0) {
                        if (out_ino) *out_ino = de->inode;
                        if (out_ft)  *out_ft  = de->file_type;
                        de->inode = 0;
                        ext4_write_block(ctx, phys, buf);
                        kfree_heap(buf);
                        return 0;
                    }
                }
                off += de->rec_len;
            }
        }
    }
    kfree_heap(buf);
    return -1;
}

int ext4_dir_empty(struct ext4_ctx* ctx, uint32_t ino) {
    struct ext4_inode di;
    ext4_read_inode(ctx, ino, &di);
    struct ext4_extent_header* eh = (struct ext4_extent_header*)di.i_block;
    if (eh->eh_magic != 0xF30A) return 1;

    uint8_t* buf = (uint8_t*)kmalloc(ctx->block_size);
    if (!buf) return 1;
    struct ext4_extent* ee = (struct ext4_extent*)((uint8_t*)di.i_block + sizeof(*eh));
    uint16_t nr = eh->eh_entries;
    if (nr > EXT4_MAX_EXTENTS) nr = EXT4_MAX_EXTENTS;
    for (uint16_t ei = 0; ei < nr; ei++) {
        uint16_t elen = ee[ei].ee_len;
        if (elen > 32768) elen = 32768;
        for (uint32_t bi = 0; bi < elen; bi++) {
            memory_set(buf, 0, ctx->block_size);
            ext4_read_block(ctx, ee[ei].ee_start_lo + bi, buf);
            uint32_t off = 0;
            while (off + sizeof(struct ext4_dir_entry_2) <= ctx->block_size) {
                struct ext4_dir_entry_2* de = (struct ext4_dir_entry_2*)(buf + off);
                if (!de->rec_len || de->rec_len < sizeof(struct ext4_dir_entry_2)) break;
                if (off + de->rec_len > ctx->block_size) break;
                if (de->inode && de->name_len) {
                    char en[256];
                    memory_copy(en, de->name, de->name_len);
                    en[de->name_len] = '\0';
                    if (compare_string(en, ".") != 0 && compare_string(en, "..") != 0) {
                        kfree_heap(buf);
                        return 0;
                    }
                }
                off += de->rec_len;
            }
        }
    }
    kfree_heap(buf);
    return 1;
}
