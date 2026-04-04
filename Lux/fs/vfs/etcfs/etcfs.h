#ifndef ETCFS_H
#define ETCFS_H

#include <stdint.h>
#include "vfs.h"


#define ETCFS_NAME_LEN   128
#define ETCFS_INIT_SIZE  4096   


//Public api
void         etcfs_init    (vfs_node_t *ext4_root);
vfs_node_t  *etcfs_get_root(void);

int  etcfs_read  (const char *name, char *buf, uint32_t size);
int  etcfs_write (const char *name, const char *buf, uint32_t size);
int  etcfs_create(const char *name);
int  etcfs_delete(const char *name);
void etcfs_flush       (void);

void         etcfs_seed_users  (void);
const char  *etcfs_uid_to_name (uint32_t uid);
uint32_t     etcfs_name_to_uid (const char *name);
uint32_t     etcfs_name_to_gid (const char *name);

#endif 