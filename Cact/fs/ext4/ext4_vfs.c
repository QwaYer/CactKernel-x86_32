#include "ext4_internal.h"
#include "ext4.h"
#include "blkdev.h"
#include "klib.h"
#include "memory.h"
#include "kernel.h"
#include "pagecache.h"


// Forward declaration: populate a VFS node from an inode number and file type
static void ext4_make_node(struct ext4_ctx* ctx, vfs_node_t* n, uint32_t ino, uint8_t ft);


// ── readdir callback context 
typedef struct {
    uint32_t target_idx;
    uint32_t cur;
    vfs_dirent_t de;
    int found;
} ext4_rdctx_t;

// readdir callback: skip . and .., match by index
static void ext4_readdir_cb(struct ext4_dir_entry_2* de, void* ud) {
    ext4_rdctx_t* rc = (ext4_rdctx_t*)ud;
    if (rc->found) return;
    char name[256];
    memory_copy(name, de->name, de->name_len);
    name[de->name_len] = '\0';
    if (name[0] == '.' && (name[1] == '\0' || (name[1] == '.' && name[2] == '\0'))) return;
    if (rc->cur++ == rc->target_idx) {
        uint32_t copy_len = de->name_len;
        if (copy_len >= sizeof(rc->de.name)) copy_len = sizeof(rc->de.name) - 1;
        memory_copy(rc->de.name, name, copy_len);
        rc->de.name[copy_len] = '\0';
        rc->de.inode = de->inode;
        rc->found    = 1;
    }
}

// Return the directory entry at a given index (0-based, skips . and ..)
static vfs_dirent_t* ext4_readdir(vfs_node_t* node, uint32_t index) {
    static ext4_rdctx_t rc;
    rc.target_idx = index;
    rc.cur        = 0;
    rc.found      = 0;
    memory_set(rc.de.name, 0, 128);
    struct ext4_ctx* ctx = (struct ext4_ctx*)node->priv;
    ext4_dir_iter(ctx, node, ext4_readdir_cb, &rc);
    return rc.found ? &rc.de : 0;
}

// VFS operation thunks
static vfs_node_t* ext4_ops_walk  (vfs_node_t* d, const char* n) { return ext4_finddir(d, (char*)n); }
static int         ext4_ops_create(vfs_node_t* d, const char* n) { return ext4_create(d, (char*)n); }
static int         ext4_ops_delete(vfs_node_t* d, const char* n) { return ext4_delete(d, (char*)n); }
static int         ext4_ops_mkdir (vfs_node_t* d, const char* n) { return ext4_mkdir(d, (char*)n); }
static int         ext4_ops_rmdir (vfs_node_t* d, const char* n) { return ext4_rmdir(d, (char*)n); }

// VFS operations for directories
static vfs_ops_t ext4_dir_ops = {
    .walk    = ext4_ops_walk,
    .readdir = ext4_readdir,
    .listdir = ext4_list_dir,
    .mkdir   = ext4_ops_mkdir,
    .rmdir   = ext4_ops_rmdir,
    .create  = ext4_ops_create,
    .delete  = ext4_ops_delete,
};

// VFS operations for regular files
static vfs_ops_t ext4_file_ops = {
    .read   = ext4_read_file,
    .write  = ext4_write_file,
    .delete = ext4_ops_delete,
};

// Populate a VFS node with inode metadata and the correct ops table
static void ext4_make_node(struct ext4_ctx* ctx, vfs_node_t* n, uint32_t ino, uint8_t ft) {
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
        ext4_read_inode(ctx, ino, &fi);
        n->size = fi.i_size_lo;
    }
}

// listdir callback 
static void ext4_listdir_cb(struct ext4_dir_entry_2* de, void* ud) {
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

// finddir callback context 
typedef struct {
    char*             target;
    vfs_node_t*       result;
    struct ext4_ctx* ctx;
} ext4_findctx_t;

// finddir callback: match by name, allocate and populate a VFS node
static void ext4_finddir_cb(struct ext4_dir_entry_2* de, void* ud) {
    ext4_findctx_t* fc = (ext4_findctx_t*)ud;
    if (fc->result) return;
    char name[256];
    memory_copy(name, de->name, de->name_len);
    name[de->name_len] = '\0';
    if (compare_string(name, fc->target) != 0) return;
    vfs_node_t* res = (vfs_node_t*)kmalloc(sizeof(*res));
    if (!res) return;
    memory_copy(res->name, name, sizeof(res->name) - 1);
    res->name[sizeof(res->name) - 1] = '\0';
    ext4_make_node(fc->ctx, res, de->inode, de->file_type);
    fc->result = res;
}

// Print directory listing (non-recursive)
void ext4_list_dir(vfs_node_t* node) {
    struct ext4_ctx* ctx = (struct ext4_ctx*)node->priv;
    ext4_dir_iter(ctx, node, ext4_listdir_cb, 0);
}

// Look up a directory entry by name; returns a heap-allocated VFS node or NULL
vfs_node_t* ext4_finddir(vfs_node_t* node, char* name) {
    struct ext4_ctx* ctx = (struct ext4_ctx*)node->priv;
    ext4_findctx_t fc;
    fc.target = name;
    fc.result = 0;
    fc.ctx    = ctx;
    ext4_dir_iter(ctx, node, ext4_finddir_cb, &fc);
    return fc.result;
}

// Read from a regular file at offset into buffer; returns bytes read
int ext4_read_file(vfs_node_t* node, uint32_t offset, uint32_t size, char* buffer) {
    struct ext4_ctx* ctx = (struct ext4_ctx*)node->priv;
    struct ext4_inode inode;
    ext4_read_inode(ctx, node->inode, &inode);
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
        uint32_t fb  = cur / ctx->block_size;
        uint32_t ibo = cur % ctx->block_size;
        uint32_t pb  = use_ext ? ext4_extent_pblock(&inode, fb)
                               : ext4_legacy_bmap(ctx, &inode, fb);
        if (!pb) break;
        memory_set(tmp, 0, ctx->block_size);
        ext4_read_block(ctx, pb, tmp);
        uint32_t tc = ctx->block_size - ibo;
        if (tc > size - read) tc = size - read;
        memory_copy(buffer + read, tmp + ibo, tc);
        read += tc;
        cur  += tc;
    }
    kfree_heap(tmp);
    return (int)read;
}

// Write to a regular file at offset from buffer; returns bytes written
int ext4_write_file(vfs_node_t* node, uint32_t offset, uint32_t size, char* buffer) {
    if (!size) return 0;
    struct ext4_ctx* ctx = (struct ext4_ctx*)node->priv;
    ext4_journal_start(ctx);

    struct ext4_inode inode;
    ext4_read_inode(ctx, node->inode, &inode);
    struct ext4_extent_header* eh = (struct ext4_extent_header*)inode.i_block;
    if (eh->eh_magic != 0xF30A) ext4_extent_init(&inode);

    uint8_t* tmp = (uint8_t*)kmalloc(ctx->block_size);
    if (!tmp) { ext4_journal_stop(ctx); return -1; }

    uint32_t written = 0, cur = offset;
    while (written < size) {
        uint32_t fb  = cur / ctx->block_size;
        uint32_t ibo = cur % ctx->block_size;
        uint32_t pb  = ext4_extent_pblock(&inode, fb);
        if (!pb) {
            pb = ext4_alloc_block(ctx);
            if (!pb) break;
            memory_set(tmp, 0, ctx->block_size);
            ext4_write_block(ctx, pb, tmp);
            if (ext4_extent_add(&inode, fb, pb, 1) < 0) {
                ext4_free_block(ctx, pb);
                break;
            }
            inode.i_blocks_lo += ctx->block_size / 512;
        }
        memory_set(tmp, 0, ctx->block_size);
        ext4_read_block(ctx, pb, tmp);
        uint32_t tc = ctx->block_size - ibo;
        if (tc > size - written) tc = size - written;
        memory_copy(tmp + ibo, buffer + written, tc);
        ext4_write_block(ctx, pb, tmp);
        written += tc;
        cur     += tc;
    }
    kfree_heap(tmp);
    if (cur > inode.i_size_lo) {
        inode.i_size_lo = cur;
        node->size      = cur;
    }
    ext4_write_inode(ctx, node->inode, &inode);
    ext4_journal_stop(ctx);
    return (int)written;
}

// Create a regular file in a directory
int ext4_create(vfs_node_t* node, char* name) {
    struct ext4_ctx* ctx = (struct ext4_ctx*)node->priv;
    ext4_journal_start(ctx);

    vfs_node_t* ex = ext4_finddir(node, name);
    if (ex) { kfree_heap(ex); ext4_journal_stop(ctx); return -1; }

    uint32_t ino = ext4_alloc_inode(ctx);
    if (!ino) { ext4_journal_stop(ctx); return -1; }

    struct ext4_inode ni;
    memory_set(&ni, 0, sizeof(ni));
    ni.i_mode        = 0x81A4;    // regular file, 0644
    ni.i_links_count = 1;
    ni.i_flags       = 0x80000;   // extents
    ext4_extent_init(&ni);
    ext4_write_inode(ctx, ino, &ni);

    if (ext4_dir_add(ctx, node, ino, name, EXT4_FT_REG_FILE) < 0) {
        ext4_free_inode(ctx, ino);
        ext4_journal_stop(ctx);
        return -1;
    }
    ext4_journal_stop(ctx);
    return 0;
}

// Create a subdirectory with . and .. entries
int ext4_mkdir(vfs_node_t* node, char* name) {
    struct ext4_ctx* ctx = (struct ext4_ctx*)node->priv;
    ext4_journal_start(ctx);

    vfs_node_t* ex = ext4_finddir(node, name);
    if (ex) { kfree_heap(ex); ext4_journal_stop(ctx); return -1; }

    uint32_t ino = ext4_alloc_inode(ctx);
    if (!ino) { ext4_journal_stop(ctx); return -1; }

    uint32_t db = ext4_alloc_block(ctx);
    if (!db) { ext4_free_inode(ctx, ino); ext4_journal_stop(ctx); return -1; }

    struct ext4_inode ni;
    memory_set(&ni, 0, sizeof(ni));
    ni.i_mode        = 0x41ED;    // directory, 0755
    ni.i_links_count = 2;         // . and parent's ..
    ni.i_size_lo     = ctx->block_size;
    ni.i_blocks_lo   = ctx->block_size / 512;
    ni.i_flags       = 0x80000;
    ext4_extent_init(&ni);
    ext4_extent_add(&ni, 0, db, 1);
    ext4_write_inode(ctx, ino, &ni);

    uint8_t* buf = (uint8_t*)kmalloc(ctx->block_size);
    if (!buf) {
        ext4_free_inode(ctx, ino);
        ext4_free_block(ctx, db);
        ext4_journal_stop(ctx);
        return -1;
    }
    memory_set(buf, 0, ctx->block_size);
    struct ext4_dir_entry_2* dot = (struct ext4_dir_entry_2*)buf;
    dot->inode     = ino;
    dot->rec_len   = 12;
    dot->name_len  = 1;
    dot->file_type = EXT4_FT_DIR;
    dot->name[0]   = '.';
    struct ext4_dir_entry_2* dd = (struct ext4_dir_entry_2*)(buf + 12);
    dd->inode     = node->inode;
    dd->rec_len   = (uint16_t)(ctx->block_size - 12);
    dd->name_len  = 2;
    dd->file_type = EXT4_FT_DIR;
    dd->name[0]   = '.';
    dd->name[1]   = '.';
    ext4_write_block(ctx, db, buf);
    kfree_heap(buf);

    struct ext4_inode pi;
    ext4_read_inode(ctx, node->inode, &pi);
    pi.i_links_count++;
    ext4_write_inode(ctx, node->inode, &pi);

    if (ext4_dir_add(ctx, node, ino, name, EXT4_FT_DIR) < 0) {
        ext4_free_inode(ctx, ino);
        ext4_free_block(ctx, db);
        ext4_journal_stop(ctx);
        return -1;
    }
    ext4_journal_stop(ctx);
    return 0;
}

// Delete a regular file (unlink)
int ext4_delete(vfs_node_t* node, char* name) {
    struct ext4_ctx* ctx = (struct ext4_ctx*)node->priv;
    ext4_journal_start(ctx);

    uint32_t del_ino = 0;
    uint8_t  del_ft  = 0;
    if (ext4_dir_remove(ctx, node, name, &del_ino, &del_ft) < 0 || !del_ino) {
        ext4_journal_stop(ctx);
        return -1;
    }
    if (del_ft == EXT4_FT_DIR) { ext4_journal_stop(ctx); return -1; }  // use rmdir

    struct ext4_inode inode;
    ext4_read_inode(ctx, del_ino, &inode);
    if (inode.i_links_count) inode.i_links_count--;
    if (!inode.i_links_count) {
        struct ext4_extent_header* eh = (struct ext4_extent_header*)inode.i_block;
        if (eh->eh_magic == 0xF30A) {
            uint16_t nr = eh->eh_entries;
            if (nr > EXT4_MAX_EXTENTS) nr = EXT4_MAX_EXTENTS;
            struct ext4_extent* ee = (struct ext4_extent*)((uint8_t*)inode.i_block + sizeof(*eh));
            for (uint16_t i = 0; i < nr; i++)
                for (uint32_t b = 0; b < ee[i].ee_len; b++)
                    ext4_free_block(ctx, ee[i].ee_start_lo + b);
        }
        inode.i_dtime = 1;
        ext4_write_inode(ctx, del_ino, &inode);
        ext4_free_inode(ctx, del_ino);
    } else {
        ext4_write_inode(ctx, del_ino, &inode);
    }
    ext4_journal_stop(ctx);
    return 0;
}

// Remove an empty directory
int ext4_rmdir(vfs_node_t* node, char* name) {
    struct ext4_ctx* ctx = (struct ext4_ctx*)node->priv;
    ext4_journal_start(ctx);

    vfs_node_t* tgt = ext4_finddir(node, name);
    if (!tgt) { ext4_journal_stop(ctx); return -1; }
    if (tgt->type != VFS_DIRECTORY) { kfree_heap(tgt); ext4_journal_stop(ctx); return -1; }
    uint32_t tino = tgt->inode;
    kfree_heap(tgt);

    if (!ext4_dir_empty(ctx, tino)) { ext4_journal_stop(ctx); return -1; }

    uint32_t del_ino = 0;
    uint8_t  del_ft  = 0;
    if (ext4_dir_remove(ctx, node, name, &del_ino, &del_ft) < 0) {
        ext4_journal_stop(ctx);
        return -1;
    }

    struct ext4_inode di;
    ext4_read_inode(ctx, del_ino, &di);
    struct ext4_extent_header* eh = (struct ext4_extent_header*)di.i_block;
    if (eh->eh_magic == 0xF30A) {
        uint16_t nr = eh->eh_entries;
        if (nr > EXT4_MAX_EXTENTS) nr = EXT4_MAX_EXTENTS;
        struct ext4_extent* ee = (struct ext4_extent*)((uint8_t*)di.i_block + sizeof(*eh));
        for (uint16_t i = 0; i < nr; i++)
            for (uint32_t b = 0; b < ee[i].ee_len; b++)
                ext4_free_block(ctx, ee[i].ee_start_lo + b);
    }
    di.i_dtime = 1;
    ext4_write_inode(ctx, del_ino, &di);
    ext4_free_inode(ctx, del_ino);

    struct ext4_inode pi;
    ext4_read_inode(ctx, node->inode, &pi);
    if (pi.i_links_count > 1) pi.i_links_count--;
    ext4_write_inode(ctx, node->inode, &pi);

    ext4_journal_stop(ctx);
    return 0;
}

// Mount an ext4 filesystem from a block device ID
vfs_node_t* ext4_mount_disk(uint32_t dev) {
    struct ext4_ctx* ctx = (struct ext4_ctx*)kmalloc(sizeof(*ctx));
    if (!ctx) return 0;
    memory_set(ctx, 0, sizeof(*ctx));
    ctx->dev = dev;

    // Read superblock (primary at sector 2, backup at sector 0)
    uint8_t buf[2048];
    memory_set(buf, 0, sizeof(buf));
    blkdev_read_sector(2, buf);
    blkdev_read_sector(3, buf + 512);
    memory_copy(&ctx->sb, buf, 1024);

    if (ctx->sb.s_magic != EXT4_SUPER_MAGIC) {
        blkdev_read_sector(0, buf);
        blkdev_read_sector(1, buf + 512);
        memory_copy(&ctx->sb, buf, 1024);
    }

    if (ctx->sb.s_magic != EXT4_SUPER_MAGIC) {
        kprint("[ext4] ERROR: superblock not found\n");
        kfree_heap(ctx);
        return 0;
    }

    if (ctx->sb.s_log_block_size > 5 || ctx->sb.s_log_block_size == (uint32_t)-1) {
        kprint("[ext4] ERROR: invalid s_log_block_size\n");
        kfree_heap(ctx);
        return 0;
    }
    ctx->block_size = 1024u << ctx->sb.s_log_block_size;

    // Allocate root VFS node (inode 2)
    vfs_node_t* root = (vfs_node_t*)kmalloc(sizeof(*root));
    if (!root) { kfree_heap(ctx); return 0; }
    memory_set(root, 0, sizeof(*root));
    copy_string(root->name, "/");
    root->inode = 2;
    ext4_make_node(ctx, root, 2, EXT4_FT_DIR);
    copy_string(root->name, "/");

    // Initialise journal if present
    if (ctx->sb.s_feature_compat & 0x0004) {
        ctx->journal.j_inum = ctx->sb.s_journal_inum;
        if (ctx->journal.j_inum) {
            uint8_t* jsb = (uint8_t*)kmalloc(ctx->block_size);
            if (jsb) {
                memory_set(jsb, 0, ctx->block_size);
                ext4_jbd_read(ctx, 0, jsb);
                ctx->journal.j_sb = (struct jbd2_superblock*)jsb;
                uint32_t m  = ctx->journal.j_sb->s_header.h_magic;
                uint32_t mb = ((m & 0xff000000) >> 24) | ((m & 0x00ff0000) >> 8)
                            | ((m & 0x0000ff00) << 8) | ((m & 0x000000ff) << 24);
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
        ctx->journal.j_sb                   = 0;
        ctx->journal.j_running_transaction = 0;
    }

    return root;
}

// Boot-time initialisation: mount the root filesystem from device 0
void ext4_init(void) { vfs_root = ext4_mount_disk(0); }