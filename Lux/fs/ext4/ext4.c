#include "ext4.h"
#include "ata.h"
#include "libc.h"
#include "memory.h"
#include "kernel.h"

static struct ext4_superblock sb;
static uint32_t block_size;

void ext4_read_block(uint32_t block, uint8_t* buffer) {
    uint32_t sectors_per_block = block_size / 512;
    uint32_t lba_start = block * sectors_per_block;
    for (uint32_t i = 0; i < sectors_per_block; i++) {
        ata_read_sector(lba_start + i, buffer + (i * 512));
    }
}

static void ext4_write_block(uint32_t block, uint8_t* buffer) {
    uint32_t sectors_per_block = block_size / 512;
    uint32_t lba_start = block * sectors_per_block;
    for (uint32_t i = 0; i < sectors_per_block; i++) {
        ata_write_sector(lba_start + i, buffer + (i * 512));
    }
}

static uint32_t ext4_gd_base_block(void) {
    return (block_size == 1024) ? 2 : 1;
}

static void ext4_read_group_desc(uint32_t group, struct ext4_group_desc* gd) {
    uint32_t desc_size = sb.s_desc_size ? sb.s_desc_size : 32;
    uint32_t byte_off  = group * desc_size;
    uint32_t blk_off   = byte_off / block_size;
    uint32_t inner_off = byte_off % block_size;

    uint8_t* buf = kmalloc(block_size);
    if (!buf) return;
    memory_set(buf, 0, block_size);
    ext4_read_block(ext4_gd_base_block() + blk_off, buf);
    memory_copy(gd, buf + inner_off, sizeof(struct ext4_group_desc));
    kfree_heap(buf);
}

static void ext4_write_group_desc(uint32_t group, struct ext4_group_desc* gd) {
    uint32_t desc_size = sb.s_desc_size ? sb.s_desc_size : 32;
    uint32_t byte_off  = group * desc_size;
    uint32_t blk_off   = byte_off / block_size;
    uint32_t inner_off = byte_off % block_size;

    uint8_t* buf = kmalloc(block_size);
    if (!buf) return;
    memory_set(buf, 0, block_size);
    ext4_read_block(ext4_gd_base_block() + blk_off, buf);
    memory_copy(buf + inner_off, gd, sizeof(struct ext4_group_desc));
    ext4_write_block(ext4_gd_base_block() + blk_off, buf);
    kfree_heap(buf);
}

void ext4_read_inode(uint32_t inode_no, struct ext4_inode* inode) {
    if (!inode_no) return;

    uint32_t group = (inode_no - 1) / sb.s_inodes_per_group;
    uint32_t index = (inode_no - 1) % sb.s_inodes_per_group;

    struct ext4_group_desc gd;
    ext4_read_group_desc(group, &gd);

    uint32_t offset       = index * sb.s_inode_size;
    uint32_t block_offset = offset / block_size;
    uint32_t inner_offset = offset % block_size;

    uint8_t* inode_buf = kmalloc(block_size);
    if (!inode_buf) return;
    memory_set(inode_buf, 0, block_size);
    ext4_read_block(gd.bg_inode_table_lo + block_offset, inode_buf);
    memory_copy(inode, inode_buf + inner_offset, sizeof(struct ext4_inode));
    kfree_heap(inode_buf);
}

static void ext4_write_inode(uint32_t inode_no, struct ext4_inode* inode) {
    if (!inode_no) return;

    uint32_t group = (inode_no - 1) / sb.s_inodes_per_group;
    uint32_t index = (inode_no - 1) % sb.s_inodes_per_group;

    struct ext4_group_desc gd;
    ext4_read_group_desc(group, &gd);

    uint32_t offset       = index * sb.s_inode_size;
    uint32_t block_offset = offset / block_size;
    uint32_t inner_offset = offset % block_size;

    uint8_t* inode_buf = kmalloc(block_size);
    if (!inode_buf) return;
    memory_set(inode_buf, 0, block_size);
    ext4_read_block(gd.bg_inode_table_lo + block_offset, inode_buf);
    memory_copy(inode_buf + inner_offset, inode, sizeof(struct ext4_inode));
    ext4_write_block(gd.bg_inode_table_lo + block_offset, inode_buf);
    kfree_heap(inode_buf);
}

static void ext4_write_superblock(void) {
    uint8_t buf[1024];
    memory_set(buf, 0, sizeof(buf));
    memory_copy(buf, &sb, sizeof(struct ext4_superblock));
    ata_write_sector(2, buf);
    ata_write_sector(3, buf + 512);
}

static int bitmap_test(uint8_t* bmap, uint32_t bit) {
    return (bmap[bit / 8] >> (bit % 8)) & 1;
}
static void bitmap_set(uint8_t* bmap, uint32_t bit) {
    bmap[bit / 8] |= (uint8_t)(1 << (bit % 8));
}
static void bitmap_clear(uint8_t* bmap, uint32_t bit) {
    bmap[bit / 8] &= (uint8_t)~(1 << (bit % 8));
}

static uint32_t ext4_alloc_block_in_group(uint32_t group) {
    struct ext4_group_desc gd;
    ext4_read_group_desc(group, &gd);
    if (!gd.bg_free_blocks_count_lo) return 0;

    uint8_t* bmap = kmalloc(block_size);
    if (!bmap) return 0;
    memory_set(bmap, 0, block_size);
    ext4_read_block(gd.bg_block_bitmap_lo, bmap);

    uint32_t found = 0;
    for (uint32_t i = 0; i < sb.s_blocks_per_group; i++) {
        if (!bitmap_test(bmap, i)) {
            bitmap_set(bmap, i);
            found = i;
            goto done;
        }
    }
    kfree_heap(bmap);
    return 0;

done:
    ext4_write_block(gd.bg_block_bitmap_lo, bmap);
    kfree_heap(bmap);

    gd.bg_free_blocks_count_lo--;
    ext4_write_group_desc(group, &gd);

    sb.s_free_blocks_count_lo--;
    ext4_write_superblock();

    return sb.s_first_data_block + group * sb.s_blocks_per_group + found;
}

static uint32_t ext4_alloc_block(void) {
    uint32_t groups = (sb.s_blocks_count_lo + sb.s_blocks_per_group - 1) / sb.s_blocks_per_group;
    for (uint32_t g = 0; g < groups; g++) {
        uint32_t b = ext4_alloc_block_in_group(g);
        if (b) return b;
    }
    return 0;
}

static void ext4_free_block(uint32_t block_no) {
    if (!block_no) return;
    uint32_t rel   = block_no - sb.s_first_data_block;
    uint32_t group = rel / sb.s_blocks_per_group;
    uint32_t index = rel % sb.s_blocks_per_group;

    struct ext4_group_desc gd;
    ext4_read_group_desc(group, &gd);

    uint8_t* bmap = kmalloc(block_size);
    if (!bmap) return;
    memory_set(bmap, 0, block_size);
    ext4_read_block(gd.bg_block_bitmap_lo, bmap);
    bitmap_clear(bmap, index);
    ext4_write_block(gd.bg_block_bitmap_lo, bmap);
    kfree_heap(bmap);

    gd.bg_free_blocks_count_lo++;
    ext4_write_group_desc(group, &gd);
    sb.s_free_blocks_count_lo++;
    ext4_write_superblock();
}

static uint32_t ext4_alloc_inode_in_group(uint32_t group) {
    struct ext4_group_desc gd;
    ext4_read_group_desc(group, &gd);
    if (!gd.bg_free_inodes_count_lo) return 0;

    uint8_t* bmap = kmalloc(block_size);
    if (!bmap) return 0;
    memory_set(bmap, 0, block_size);
    ext4_read_block(gd.bg_inode_bitmap_lo, bmap);

    uint32_t found = 0;
    int ok = 0;
    for (uint32_t i = 0; i < sb.s_inodes_per_group; i++) {
        if (!bitmap_test(bmap, i)) {
            bitmap_set(bmap, i);
            found = i;
            ok = 1;
            break;
        }
    }
    if (!ok) { kfree_heap(bmap); return 0; }

    ext4_write_block(gd.bg_inode_bitmap_lo, bmap);
    kfree_heap(bmap);

    gd.bg_free_inodes_count_lo--;
    ext4_write_group_desc(group, &gd);
    sb.s_free_inodes_count--;
    ext4_write_superblock();

    return group * sb.s_inodes_per_group + found + 1;
}

static uint32_t ext4_alloc_inode(void) {
    uint32_t groups = (sb.s_blocks_count_lo + sb.s_blocks_per_group - 1) / sb.s_blocks_per_group;
    for (uint32_t g = 0; g < groups; g++) {
        uint32_t ino = ext4_alloc_inode_in_group(g);
        if (ino) return ino;
    }
    return 0;
}

static void ext4_free_inode(uint32_t inode_no) {
    if (!inode_no) return;
    uint32_t group = (inode_no - 1) / sb.s_inodes_per_group;
    uint32_t index = (inode_no - 1) % sb.s_inodes_per_group;

    struct ext4_group_desc gd;
    ext4_read_group_desc(group, &gd);

    uint8_t* bmap = kmalloc(block_size);
    if (!bmap) return;
    memory_set(bmap, 0, block_size);
    ext4_read_block(gd.bg_inode_bitmap_lo, bmap);
    bitmap_clear(bmap, index);
    ext4_write_block(gd.bg_inode_bitmap_lo, bmap);
    kfree_heap(bmap);

    gd.bg_free_inodes_count_lo++;
    ext4_write_group_desc(group, &gd);
    sb.s_free_inodes_count++;
    ext4_write_superblock();
}

static uint32_t ext4_extent_get_pblock(struct ext4_inode* inode, uint32_t file_block) {
    struct ext4_extent_header* eh = (struct ext4_extent_header*)inode->i_block;
    if (eh->eh_magic != 0xF30A || eh->eh_depth != 0) return 0;

    struct ext4_extent* ee = (struct ext4_extent*)((uint8_t*)inode->i_block + sizeof(struct ext4_extent_header));
    for (uint16_t i = 0; i < eh->eh_entries; i++) {
        if (file_block >= ee[i].ee_block && file_block < ee[i].ee_block + ee[i].ee_len)
            return ee[i].ee_start_lo + (file_block - ee[i].ee_block);
    }
    return 0;
}

static void ext4_init_extent_header(struct ext4_inode* inode) {
    memory_set(inode->i_block, 0, sizeof(inode->i_block));
    struct ext4_extent_header* eh = (struct ext4_extent_header*)inode->i_block;
    eh->eh_magic   = 0xF30A;
    eh->eh_entries = 0;
    eh->eh_max     = 4;
    eh->eh_depth   = 0;
}

static int ext4_inode_add_extent(struct ext4_inode* inode, uint32_t file_block,
                                  uint32_t phys_block, uint16_t len) {
    struct ext4_extent_header* eh = (struct ext4_extent_header*)inode->i_block;
    if (eh->eh_entries >= eh->eh_max) return -1;

    struct ext4_extent* ee = (struct ext4_extent*)((uint8_t*)inode->i_block + sizeof(struct ext4_extent_header));
    uint16_t idx = eh->eh_entries;
    ee[idx].ee_block    = file_block;
    ee[idx].ee_len      = len;
    ee[idx].ee_start_hi = 0;
    ee[idx].ee_start_lo = phys_block;
    eh->eh_entries++;
    return 0;
}

static void ext4_iterate_dir(struct vfs_node* node,
                              void (*callback)(struct ext4_dir_entry_2*, void*),
                              void* userdata) {
    struct ext4_inode inode;
    ext4_read_inode(node->inode, &inode);

    uint8_t* dir_buf = kmalloc(block_size);
    if (!dir_buf) return;

    struct ext4_extent_header* eh = (struct ext4_extent_header*)inode.i_block;

    if (eh->eh_magic == 0xF30A) {
        struct ext4_extent* extents = (struct ext4_extent*)((uint8_t*)inode.i_block + sizeof(struct ext4_extent_header));
        for (uint16_t ei = 0; ei < eh->eh_entries; ei++) {
            for (uint32_t bi = 0; bi < extents[ei].ee_len; bi++) {
                memory_set(dir_buf, 0, block_size);
                ext4_read_block(extents[ei].ee_start_lo + bi, dir_buf);

                uint32_t off = 0;
                while (off + sizeof(struct ext4_dir_entry_2) <= block_size) {
                    struct ext4_dir_entry_2* de = (struct ext4_dir_entry_2*)(dir_buf + off);
                    if (de->rec_len == 0) break;
                    if (de->inode != 0 && de->name_len > 0) callback(de, userdata);
                    off += de->rec_len;
                }
            }
        }
    } else {
        for (int bi = 0; bi < 12; bi++) {
            if (!inode.i_block[bi]) break;
            memory_set(dir_buf, 0, block_size);
            ext4_read_block(inode.i_block[bi], dir_buf);

            uint32_t off = 0;
            while (off + sizeof(struct ext4_dir_entry_2) <= block_size) {
                struct ext4_dir_entry_2* de = (struct ext4_dir_entry_2*)(dir_buf + off);
                if (de->rec_len == 0) break;
                if (de->inode != 0 && de->name_len > 0) callback(de, userdata);
                off += de->rec_len;
            }
        }
    }

    kfree_heap(dir_buf);
}

static int ext4_dir_add_entry(struct vfs_node* node, uint32_t entry_inode,
                               const char* name, uint8_t file_type) {
    struct ext4_inode dir_inode;
    ext4_read_inode(node->inode, &dir_inode);

    struct ext4_extent_header* eh = (struct ext4_extent_header*)dir_inode.i_block;
    if (eh->eh_magic != 0xF30A) {
        ext4_init_extent_header(&dir_inode);
        eh = (struct ext4_extent_header*)dir_inode.i_block;
    }

    uint8_t name_len = 0;
    while (name[name_len]) name_len++;
    uint16_t needed = (uint16_t)((sizeof(struct ext4_dir_entry_2) + name_len + 3) & ~3u);

    uint8_t* dir_buf = kmalloc(block_size);
    if (!dir_buf) return -1;

    struct ext4_extent* extents = (struct ext4_extent*)((uint8_t*)dir_inode.i_block + sizeof(struct ext4_extent_header));

    for (uint16_t ei = 0; ei < eh->eh_entries; ei++) {
        for (uint32_t bi = 0; bi < extents[ei].ee_len; bi++) {
            uint32_t phys = extents[ei].ee_start_lo + bi;
            memory_set(dir_buf, 0, block_size);
            ext4_read_block(phys, dir_buf);

            uint32_t off = 0;
            while (off + sizeof(struct ext4_dir_entry_2) <= block_size) {
                struct ext4_dir_entry_2* de = (struct ext4_dir_entry_2*)(dir_buf + off);
                if (de->rec_len == 0) break;

                uint16_t real = de->inode
                    ? (uint16_t)((sizeof(struct ext4_dir_entry_2) + de->name_len + 3) & ~3u)
                    : 0;
                uint16_t free_space = de->rec_len - real;

                if (!de->inode && de->rec_len >= needed) {
                    de->inode     = entry_inode;
                    de->name_len  = name_len;
                    de->file_type = file_type;
                    memory_copy(de->name, name, name_len);
                    ext4_write_block(phys, dir_buf);
                    kfree_heap(dir_buf);
                    return 0;
                }

                if (de->inode && free_space >= needed) {
                    uint16_t old_rec = de->rec_len;
                    de->rec_len = real;

                    struct ext4_dir_entry_2* nd = (struct ext4_dir_entry_2*)(dir_buf + off + real);
                    nd->inode     = entry_inode;
                    nd->rec_len   = old_rec - real;
                    nd->name_len  = name_len;
                    nd->file_type = file_type;
                    memory_copy(nd->name, name, name_len);
                    ext4_write_block(phys, dir_buf);
                    kfree_heap(dir_buf);
                    return 0;
                }

                off += de->rec_len;
            }
        }
    }

    uint32_t new_phys = ext4_alloc_block();
    if (!new_phys) { kfree_heap(dir_buf); return -1; }

    memory_set(dir_buf, 0, block_size);
    struct ext4_dir_entry_2* de = (struct ext4_dir_entry_2*)dir_buf;
    de->inode     = entry_inode;
    de->rec_len   = (uint16_t)block_size;
    de->name_len  = name_len;
    de->file_type = file_type;
    memory_copy(de->name, name, name_len);
    ext4_write_block(new_phys, dir_buf);
    kfree_heap(dir_buf);

    uint32_t new_fb = 0;
    for (uint16_t ei = 0; ei < eh->eh_entries; ei++) new_fb += extents[ei].ee_len;

    if (ext4_inode_add_extent(&dir_inode, new_fb, new_phys, 1) < 0) {
        ext4_free_block(new_phys);
        return -1;
    }

    dir_inode.i_size_lo  += block_size;
    dir_inode.i_blocks_lo += block_size / 512;
    ext4_write_inode(node->inode, &dir_inode);
    return 0;
}

static int ext4_dir_remove_entry(struct vfs_node* node, const char* name,
                                  uint32_t* out_inode, uint8_t* out_type) {
    struct ext4_inode dir_inode;
    ext4_read_inode(node->inode, &dir_inode);

    struct ext4_extent_header* eh = (struct ext4_extent_header*)dir_inode.i_block;
    if (eh->eh_magic != 0xF30A) return -1;

    uint8_t name_len = 0;
    while (name[name_len]) name_len++;

    uint8_t* dir_buf = kmalloc(block_size);
    if (!dir_buf) return -1;

    struct ext4_extent* extents = (struct ext4_extent*)((uint8_t*)dir_inode.i_block + sizeof(struct ext4_extent_header));
    for (uint16_t ei = 0; ei < eh->eh_entries; ei++) {
        for (uint32_t bi = 0; bi < extents[ei].ee_len; bi++) {
            uint32_t phys = extents[ei].ee_start_lo + bi;
            memory_set(dir_buf, 0, block_size);
            ext4_read_block(phys, dir_buf);

            uint32_t off = 0;
            while (off + sizeof(struct ext4_dir_entry_2) <= block_size) {
                struct ext4_dir_entry_2* de = (struct ext4_dir_entry_2*)(dir_buf + off);
                if (de->rec_len == 0) break;

                if (de->inode && de->name_len == name_len) {
                    char en[256];
                    memory_copy(en, de->name, de->name_len);
                    en[de->name_len] = '\0';
                    if (compare_string(en, name) == 0) {
                        if (out_inode) *out_inode = de->inode;
                        if (out_type)  *out_type  = de->file_type;
                        de->inode = 0; 
                        ext4_write_block(phys, dir_buf);
                        kfree_heap(dir_buf);
                        return 0;
                    }
                }
                off += de->rec_len;
            }
        }
    }

    kfree_heap(dir_buf);
    return -1;
}

static int ext4_dir_is_empty(uint32_t inode_no) {
    struct ext4_inode dir_inode;
    ext4_read_inode(inode_no, &dir_inode);

    struct ext4_extent_header* eh = (struct ext4_extent_header*)dir_inode.i_block;
    if (eh->eh_magic != 0xF30A) return 1;

    uint8_t* dir_buf = kmalloc(block_size);
    if (!dir_buf) return 1;

    struct ext4_extent* extents = (struct ext4_extent*)((uint8_t*)dir_inode.i_block + sizeof(struct ext4_extent_header));
    for (uint16_t ei = 0; ei < eh->eh_entries; ei++) {
        for (uint32_t bi = 0; bi < extents[ei].ee_len; bi++) {
            memory_set(dir_buf, 0, block_size);
            ext4_read_block(extents[ei].ee_start_lo + bi, dir_buf);

            uint32_t off = 0;
            while (off + sizeof(struct ext4_dir_entry_2) <= block_size) {
                struct ext4_dir_entry_2* de = (struct ext4_dir_entry_2*)(dir_buf + off);
                if (de->rec_len == 0) break;

                if (de->inode && de->name_len) {
                    char en[256];
                    memory_copy(en, de->name, de->name_len);
                    en[de->name_len] = '\0';
                    if (compare_string(en, ".") != 0 && compare_string(en, "..") != 0) {
                        kfree_heap(dir_buf);
                        return 0;
                    }
                }
                off += de->rec_len;
            }
        }
    }

    kfree_heap(dir_buf);
    return 1;
}

int ext4_read_file(struct vfs_node* node, unsigned int offset, unsigned int size, char* buffer) {
    struct ext4_inode inode;
    ext4_read_inode(node->inode, &inode);

    uint32_t file_size = inode.i_size_lo;
    if (offset >= file_size) return 0;
    if (offset + size > file_size) size = file_size - offset;
    if (!size) return 0;

    uint8_t* tmp = kmalloc(block_size);
    if (!tmp) return -1;

    struct ext4_extent_header* eh = (struct ext4_extent_header*)inode.i_block;
    int use_extents = (eh->eh_magic == 0xF30A);

    uint32_t read = 0, cur = offset;
    while (read < size) {
        uint32_t fb  = cur / block_size;
        uint32_t ibo = cur % block_size;
        uint32_t pb  = 0;

        if (use_extents)
            pb = ext4_extent_get_pblock(&inode, fb);
        else if (fb < 12)
            pb = inode.i_block[fb];

        if (!pb) break;

        memory_set(tmp, 0, block_size);
        ext4_read_block(pb, tmp);

        uint32_t tc = block_size - ibo;
        if (tc > size - read) tc = size - read;
        memory_copy(buffer + read, tmp + ibo, tc);
        read += tc;
        cur  += tc;
    }

    kfree_heap(tmp);
    return (int)read;
}

int ext4_write_file(struct vfs_node* node, unsigned int offset, unsigned int size, char* buffer) {
    if (!size) return 0;

    struct ext4_inode inode;
    ext4_read_inode(node->inode, &inode);

    struct ext4_extent_header* eh = (struct ext4_extent_header*)inode.i_block;
    if (eh->eh_magic != 0xF30A) {
        ext4_init_extent_header(&inode);
    }

    uint8_t* tmp = kmalloc(block_size);
    if (!tmp) return -1;

    uint32_t written = 0, cur = offset;
    while (written < size) {
        uint32_t fb  = cur / block_size;
        uint32_t ibo = cur % block_size;

        uint32_t pb = ext4_extent_get_pblock(&inode, fb);
        if (!pb) {
            pb = ext4_alloc_block();
            if (!pb) break;

            memory_set(tmp, 0, block_size);
            ext4_write_block(pb, tmp);

            if (ext4_inode_add_extent(&inode, fb, pb, 1) < 0) {
                ext4_free_block(pb);
                break;
            }
            inode.i_blocks_lo += block_size / 512;
        }

        memory_set(tmp, 0, block_size);
        ext4_read_block(pb, tmp);

        uint32_t tc = block_size - ibo;
        if (tc > size - written) tc = size - written;
        memory_copy(tmp + ibo, buffer + written, tc);
        ext4_write_block(pb, tmp);

        written += tc;
        cur     += tc;
    }

    kfree_heap(tmp);

    if (cur > inode.i_size_lo) {
        inode.i_size_lo = cur;
        node->size = cur;
    }

    ext4_write_inode(node->inode, &inode);
    return (int)written;
}

static void listdir_cb(struct ext4_dir_entry_2* de, void* userdata) {
    char entry_name[256];
    memory_copy(entry_name, de->name, de->name_len);
    entry_name[de->name_len] = '\0';

    if (de->file_type == EXT4_FT_DIR) {
        kprint_color(entry_name, COLOR_LIGHT_CYAN);
        kprint("/\n");
    } else {
        kprint(entry_name);
        kprint("\n");
    }
}

void ext4_list_dir(struct vfs_node* node) {
    ext4_iterate_dir(node, listdir_cb, 0);
}

typedef struct {
    char*            target;
    struct vfs_node* result;
} finddir_ctx_t;

static void finddir_cb(struct ext4_dir_entry_2* de, void* userdata) {
    finddir_ctx_t* ctx = (finddir_ctx_t*)userdata;
    if (ctx->result) return;

    char entry_name[256];
    memory_copy(entry_name, de->name, de->name_len);
    entry_name[de->name_len] = '\0';
    if (compare_string(entry_name, ctx->target) != 0) return;

    struct vfs_node* res = kmalloc(sizeof(struct vfs_node));
    if (!res) return;
    memory_set(res, 0, sizeof(struct vfs_node));

    copy_string(res->name, entry_name);
    res->inode = de->inode;

    if (de->file_type == EXT4_FT_DIR) {
        res->type    = VFS_DIRECTORY;
        res->finddir = ext4_finddir;
        res->listdir = ext4_list_dir;
        res->mkdir   = ext4_mkdir;
        res->rmdir   = ext4_rmdir;
        res->create  = ext4_create;
        res->delete  = ext4_delete;
    } else {
        res->type   = VFS_FILE;
        res->read   = ext4_read_file;
        res->write  = ext4_write_file;
        res->delete = ext4_delete;

        struct ext4_inode fi;
        ext4_read_inode(de->inode, &fi);
        res->size = fi.i_size_lo;
    }

    ctx->result = res;
}

struct vfs_node* ext4_finddir(struct vfs_node* node, char* name) {
    finddir_ctx_t ctx;
    ctx.target = name;
    ctx.result = 0;
    ext4_iterate_dir(node, finddir_cb, &ctx);
    return ctx.result;
}

int ext4_create(struct vfs_node* node, char* name) {
    struct vfs_node* ex = ext4_finddir(node, name);
    if (ex) { kfree_heap(ex); return -1; }

    uint32_t new_ino = ext4_alloc_inode();
    if (!new_ino) return -1;

    struct ext4_inode new_inode;
    memory_set(&new_inode, 0, sizeof(struct ext4_inode));
    new_inode.i_mode        = 0x81A4; 
    new_inode.i_links_count = 1;
    new_inode.i_flags       = 0x80000; 
    ext4_init_extent_header(&new_inode);
    ext4_write_inode(new_ino, &new_inode);

    if (ext4_dir_add_entry(node, new_ino, name, EXT4_FT_REG_FILE) < 0) {
        ext4_free_inode(new_ino);
        return -1;
    }
    return 0;
}

int ext4_mkdir(struct vfs_node* node, char* name) {
    struct vfs_node* ex = ext4_finddir(node, name);
    if (ex) { kfree_heap(ex); return -1; }

    uint32_t new_ino = ext4_alloc_inode();
    if (!new_ino) return -1;

    uint32_t dir_block = ext4_alloc_block();
    if (!dir_block) { ext4_free_inode(new_ino); return -1; }

    struct ext4_inode new_inode;
    memory_set(&new_inode, 0, sizeof(struct ext4_inode));
    new_inode.i_mode        = 0x41ED; 
    new_inode.i_links_count = 2;
    new_inode.i_size_lo     = block_size;
    new_inode.i_blocks_lo   = block_size / 512;
    new_inode.i_flags       = 0x80000;
    ext4_init_extent_header(&new_inode);
    ext4_inode_add_extent(&new_inode, 0, dir_block, 1);
    ext4_write_inode(new_ino, &new_inode);

    uint8_t* dir_buf = kmalloc(block_size);
    if (!dir_buf) { ext4_free_inode(new_ino); ext4_free_block(dir_block); return -1; }
    memory_set(dir_buf, 0, block_size);

    struct ext4_dir_entry_2* dot = (struct ext4_dir_entry_2*)dir_buf;
    dot->inode     = new_ino;
    dot->rec_len   = 12;
    dot->name_len  = 1;
    dot->file_type = EXT4_FT_DIR;
    dot->name[0]   = '.';

    struct ext4_dir_entry_2* dotdot = (struct ext4_dir_entry_2*)(dir_buf + 12);
    dotdot->inode     = node->inode;
    dotdot->rec_len   = (uint16_t)(block_size - 12);
    dotdot->name_len  = 2;
    dotdot->file_type = EXT4_FT_DIR;
    dotdot->name[0]   = '.';
    dotdot->name[1]   = '.';

    ext4_write_block(dir_block, dir_buf);
    kfree_heap(dir_buf);

    struct ext4_inode parent;
    ext4_read_inode(node->inode, &parent);
    parent.i_links_count++;
    ext4_write_inode(node->inode, &parent);

    if (ext4_dir_add_entry(node, new_ino, name, EXT4_FT_DIR) < 0) {
        ext4_free_inode(new_ino);
        ext4_free_block(dir_block);
        return -1;
    }
    return 0;
}


int ext4_delete(struct vfs_node* node, char* name) {
    uint32_t del_ino  = 0;
    uint8_t  del_type = 0;

    if (ext4_dir_remove_entry(node, name, &del_ino, &del_type) < 0) return -1;
    if (!del_ino) return -1;

    if (del_type == EXT4_FT_DIR) return -1;

    struct ext4_inode inode;
    ext4_read_inode(del_ino, &inode);
    if (inode.i_links_count > 0) inode.i_links_count--;

    if (inode.i_links_count == 0) {
        struct ext4_extent_header* eh = (struct ext4_extent_header*)inode.i_block;
        if (eh->eh_magic == 0xF30A) {
            struct ext4_extent* extents = (struct ext4_extent*)((uint8_t*)inode.i_block + sizeof(struct ext4_extent_header));
            for (uint16_t i = 0; i < eh->eh_entries; i++)
                for (uint32_t b = 0; b < extents[i].ee_len; b++)
                    ext4_free_block(extents[i].ee_start_lo + b);
        }
        inode.i_dtime = 1;
        ext4_write_inode(del_ino, &inode);
        ext4_free_inode(del_ino);
    } else {
        ext4_write_inode(del_ino, &inode);
    }
    return 0;
}

int ext4_rmdir(struct vfs_node* node, char* name) {
    struct vfs_node* target = ext4_finddir(node, name);
    if (!target) return -1;
    if (target->type != VFS_DIRECTORY) { kfree_heap(target); return -1; }

    uint32_t target_ino = target->inode;
    kfree_heap(target);

    if (!ext4_dir_is_empty(target_ino)) return -1;

    uint32_t del_ino  = 0;
    uint8_t  del_type = 0;
    if (ext4_dir_remove_entry(node, name, &del_ino, &del_type) < 0) return -1;

    struct ext4_inode dir_inode;
    ext4_read_inode(del_ino, &dir_inode);

    struct ext4_extent_header* eh = (struct ext4_extent_header*)dir_inode.i_block;
    if (eh->eh_magic == 0xF30A) {
        struct ext4_extent* extents = (struct ext4_extent*)((uint8_t*)dir_inode.i_block + sizeof(struct ext4_extent_header));
        for (uint16_t i = 0; i < eh->eh_entries; i++)
            for (uint32_t b = 0; b < extents[i].ee_len; b++)
                ext4_free_block(extents[i].ee_start_lo + b);
    }

    dir_inode.i_dtime = 1;
    ext4_write_inode(del_ino, &dir_inode);
    ext4_free_inode(del_ino);

    struct ext4_inode parent;
    ext4_read_inode(node->inode, &parent);
    if (parent.i_links_count > 1) parent.i_links_count--;
    ext4_write_inode(node->inode, &parent);

    return 0;
}


void ext4_init() {
    uint8_t buf[2048];
    memory_set(buf, 0, sizeof(buf));

    ata_read_sector(2, buf);
    ata_read_sector(3, buf + 512);
    memory_copy(&sb, buf, 1024);

    if (sb.s_magic != EXT4_SUPER_MAGIC) {
        ata_read_sector(0, buf);
        ata_read_sector(1, buf + 512);
        memory_copy(&sb, buf, 1024);
    }

    if (sb.s_magic != EXT4_SUPER_MAGIC) {
        kprint("[ext4] ERROR: superblock not found!\n");
        return;
    }

    block_size = 1024u << sb.s_log_block_size;

    vfs_root = (struct vfs_node*)kmalloc(sizeof(struct vfs_node));
    memory_set(vfs_root, 0, sizeof(struct vfs_node));
    copy_string(vfs_root->name, "/");
    vfs_root->type    = VFS_DIRECTORY;
    vfs_root->inode   = 2;
    vfs_root->finddir = ext4_finddir;
    vfs_root->listdir = ext4_list_dir;
    vfs_root->mkdir   = ext4_mkdir;
    vfs_root->rmdir   = ext4_rmdir;
    vfs_root->create  = ext4_create;
    vfs_root->delete  = ext4_delete;
}