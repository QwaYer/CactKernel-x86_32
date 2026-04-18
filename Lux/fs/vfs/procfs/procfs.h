#ifndef PROCFS_H
#define PROCFS_H

#include <stdint.h>
#include "vfs.h"


typedef int (*procfs_read_fn)(uint32_t off, uint32_t size, char *buf);

typedef int (*procfs_cmd_fn)(const char *cmd, uint32_t len);


//Public api
void         procfs_init        (void);
void         procfs_set_meminfo  (uint32_t mem_total_kb);
vfs_node_t  *procfs_get_root(void);

int procfs_register_file(const char *name, procfs_read_fn read_fn);

int procfs_register_cmd (const char *name,
                          procfs_read_fn read_fn,
                          procfs_cmd_fn  cmd_fn);

int procfs_unregister_file(const char *name);
int procfs_unregister_cmd (const char *name);

#endif