#ifndef EXT4_INTERNAL_H
#define EXT4_INTERNAL_H

#include "ext4.h"

void     ext4_read_block(struct ext4_ctx* ctx, uint32_t block, uint8_t* buf);
void     ext4_write_block(struct ext4_ctx* ctx, uint32_t block, uint8_t* buf);
void     ext4_journal_log(struct ext4_ctx* ctx, uint32_t blocknr, uint8_t* data);
void     ext4_journal_start(struct ext4_ctx* ctx);
void     ext4_journal_stop(struct ext4_ctx* ctx);
void     ext4_write_sb(struct ext4_ctx* ctx);
uint32_t ext4_gd_base(struct ext4_ctx* ctx);
void     ext4_read_gd(struct ext4_ctx* ctx, uint32_t group, struct ext4_group_desc* gd);
void     ext4_write_gd(struct ext4_ctx* ctx, uint32_t group, struct ext4_group_desc* gd);
void     ext4_read_inode(struct ext4_ctx* ctx, uint32_t ino, struct ext4_inode* out);
void     ext4_write_inode(struct ext4_ctx* ctx, uint32_t ino, struct ext4_inode* in);

uint32_t ext4_alloc_block(struct ext4_ctx* ctx);
void     ext4_free_block(struct ext4_ctx* ctx, uint32_t block);
uint32_t ext4_alloc_inode(struct ext4_ctx* ctx);
void     ext4_free_inode(struct ext4_ctx* ctx, uint32_t ino);

void     ext4_extent_init(struct ext4_inode* inode);
uint32_t ext4_extent_pblock(struct ext4_inode* inode, uint32_t fb);
int      ext4_extent_add(struct ext4_inode* inode, uint32_t fb, uint32_t pb, uint16_t len);
uint32_t ext4_legacy_bmap(struct ext4_ctx* ctx, struct ext4_inode* inode, uint32_t fb);

void ext4_dir_iter(struct ext4_ctx* ctx, vfs_node_t* node,
                   void (*cb)(struct ext4_dir_entry_2*, void*), void* ud);

int ext4_dir_add(struct ext4_ctx* ctx, vfs_node_t* node,
                 uint32_t entry_ino, const char* name, uint8_t ft);
int ext4_dir_remove(struct ext4_ctx* ctx, vfs_node_t* node,
                    const char* name, uint32_t* out_ino, uint8_t* out_ft);
int ext4_dir_empty(struct ext4_ctx* ctx, uint32_t ino);

uint32_t ext4_jbd_read(struct ext4_ctx* ctx, uint32_t jblock, uint8_t* buf);

#endif
