#ifndef ETCFS_H
#define ETCFS_H

#include <stdint.h>
#include "vfs.h"

// Limits
#define ETCFS_NAME_LEN   128
#define ETCFS_INIT_SIZE  4096    // initial capacity for new files

// Initialise etcfs, bind to ext4 root, load existing files, seed users
void         etcfs_init    (vfs_node_t *ext4_root);

// Return the etcfs root VFS node (to be registered in mount table)
vfs_node_t  *etcfs_get_root(void);

// Read/write/create/delete files in /etc
int  etcfs_read  (const char *name, char *buf, uint32_t size);
int  etcfs_write (const char *name, const char *buf, uint32_t size);
int  etcfs_create(const char *name);
int  etcfs_delete(const char *name);

// Flush all dirty entries to disk
void etcfs_flush       (void);

// Seed default /etc/passwd and /etc/group if missing
void         etcfs_seed_users  (void);

// User database lookups
const char  *etcfs_uid_to_name (uint32_t uid);
uint32_t     etcfs_name_to_uid (const char *name);
uint32_t     etcfs_name_to_gid (const char *name);

#endif