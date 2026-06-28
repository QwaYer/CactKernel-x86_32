#ifndef PROCFS_H
#define PROCFS_H

#include <stdint.h>
#include "vfs.h"

// Read callback: generates up to 'size' bytes at offset 'off' into 'buf'
typedef int (*procfs_read_fn)(uint32_t off, uint32_t size, char *buf);

// Initialise procfs and register default files
void         procfs_init        (void);

// Set total memory (called by mntfs during init)
void         procfs_set_meminfo  (uint32_t mem_total_kb);

// Return the procfs root VFS node (to be mounted)
vfs_node_t  *procfs_get_root(void);

// Register a read-only virtual file under /proc
int procfs_register_file(const char *name, procfs_read_fn read_fn);

// Unregister a virtual file
int procfs_unregister_file(const char *name);

#endif