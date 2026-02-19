#include "ext4.h"
#include "ata.h"
#include "libc.h"
#include "memory.h"
#include "kernel.h"

static struct ext4_superblock sb;
static uint32_t block_size;

void ext4_read_block(uint32_t block, uint8_t* buffer) {
    uint32_t sectors_per_block = block_size / 512;
    for (uint32_t i = 0; i < sectors_per_block; i++) {
        ata_read_sector(block * sectors_per_block + i, buffer + (i * 512));
    }
}

void ext4_read_inode(uint32_t inode_no, struct ext4_inode* inode) {
    uint32_t group = (inode_no - 1) / sb.s_inodes_per_group;
    uint32_t index = (inode_no - 1) % sb.s_inodes_per_group;

    uint32_t gd_block = (block_size == 1024) ? 2 : 1;

    uint8_t* gd_buf = kmalloc(block_size);
    if (!gd_buf) return;
    memory_set(gd_buf, 0, block_size);

    ext4_read_block(gd_block, gd_buf);

    struct ext4_group_desc* gd = (struct ext4_group_desc*)(gd_buf + group * sizeof(struct ext4_group_desc));
    uint32_t inode_table_block = gd->bg_inode_table_lo;

    uint32_t offset       = index * sb.s_inode_size;
    uint32_t block_offset = offset / block_size;
    uint32_t inner_offset = offset % block_size;

    uint8_t* inode_buf = kmalloc(block_size);
    if (!inode_buf) { kfree_heap(gd_buf); return; }
    memory_set(inode_buf, 0, block_size);

    ext4_read_block(inode_table_block + block_offset, inode_buf);
    memory_copy(inode, inode_buf + inner_offset, sizeof(struct ext4_inode));

    kfree_heap(gd_buf);
    kfree_heap(inode_buf);
}

static uint32_t ext4_extent_get_pblock(struct ext4_inode* inode, uint32_t file_block) {
    struct ext4_extent_header* eh = (struct ext4_extent_header*)inode->i_block;

    if (eh->eh_magic != 0xF30A) return 0;

    if (eh->eh_depth != 0) return 0;

    struct ext4_extent* ee = (struct ext4_extent*)((uint8_t*)inode->i_block + sizeof(struct ext4_extent_header));

    for (uint16_t i = 0; i < eh->eh_entries; i++) {
        uint32_t start = ee[i].ee_block;
        uint32_t len   = ee[i].ee_len;
        if (file_block >= start && file_block < start + len) {
            return ee[i].ee_start_lo + (file_block - start);
        }
    }

    return 0; /* блок не найден */
}

int ext4_read_file(struct vfs_node* node, unsigned int offset, unsigned int size, char* buffer) {
    struct ext4_inode inode;
    ext4_read_inode(node->inode, &inode);

    if (!(inode.i_flags & 0x80000)) {
        kprint("[EXT4] Only extent-based files supported\n");
        return -1;
    }

    uint8_t* tmp_buf = kmalloc(block_size);
    if (!tmp_buf) return -1;

    uint32_t bytes_read    = 0;
    uint32_t cur_offset    = offset;
    uint32_t file_size     = inode.i_size_lo;

    if (cur_offset >= file_size) { kfree_heap(tmp_buf); return 0; }
    if (cur_offset + size > file_size) size = file_size - cur_offset;

    while (bytes_read < size) {
        uint32_t file_block    = cur_offset / block_size;
        uint32_t in_block_off  = cur_offset % block_size;
        uint32_t phys_block    = ext4_extent_get_pblock(&inode, file_block);

        if (!phys_block) break;

        memory_set(tmp_buf, 0, block_size);
        ext4_read_block(phys_block, tmp_buf);

        uint32_t to_copy = block_size - in_block_off;
        if (to_copy > size - bytes_read) to_copy = size - bytes_read;

        memory_copy(buffer + bytes_read, tmp_buf + in_block_off, to_copy);
        bytes_read += to_copy;
        cur_offset += to_copy;
    }

    kfree_heap(tmp_buf);
    return (int)bytes_read;
}

static void ext4_iterate_dir(struct vfs_node* node,
                              void (*callback)(struct ext4_dir_entry_2*, void*),
                              void* userdata) {
    struct ext4_inode inode;
    ext4_read_inode(node->inode, &inode);

    struct ext4_extent_header* eh = (struct ext4_extent_header*)inode.i_block;
    if (eh->eh_magic != 0xF30A) return;

    struct ext4_extent* extents = (struct ext4_extent*)((uint8_t*)inode.i_block + sizeof(struct ext4_extent_header));

    uint8_t* dir_buf = kmalloc(block_size);
    if (!dir_buf) return;

    for (uint16_t ei = 0; ei < eh->eh_entries; ei++) {
        for (uint32_t bi = 0; bi < extents[ei].ee_len; bi++) {
            uint32_t phys_block = extents[ei].ee_start_lo + bi;
            memory_set(dir_buf, 0, block_size);
            ext4_read_block(phys_block, dir_buf);

            uint32_t off = 0;
            while (off < block_size) {
                struct ext4_dir_entry_2* de = (struct ext4_dir_entry_2*)(dir_buf + off);
                if (de->rec_len == 0) break;
                if (de->inode != 0 && de->name_len > 0) {
                    callback(de, userdata);
                }
                off += de->rec_len;
            }
        }
    }

    kfree_heap(dir_buf);
}

static void listdir_cb(struct ext4_dir_entry_2* de, void* userdata) {
    char entry_name[256];
    memory_copy(entry_name, de->name, de->name_len);
    entry_name[de->name_len] = '\0';

    if (de->file_type == 2) {
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

    if (de->file_type == 2) {
        res->type    = VFS_DIRECTORY;
        res->finddir = ext4_finddir;
        res->listdir = ext4_list_dir;
    } else {
        res->type = VFS_FILE;
        res->read = ext4_read_file;

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

    if (sb.s_magic != EXT4_SUPER_MAGIC) return;

    block_size = 1024 << sb.s_log_block_size;

    vfs_root = (struct vfs_node*)kmalloc(sizeof(struct vfs_node));
    memory_set(vfs_root, 0, sizeof(struct vfs_node));
    copy_string(vfs_root->name, "/");
    vfs_root->type = VFS_DIRECTORY;
    vfs_root->inode = 2; 
    vfs_root->finddir = ext4_finddir;
    vfs_root->listdir = ext4_list_dir;
}