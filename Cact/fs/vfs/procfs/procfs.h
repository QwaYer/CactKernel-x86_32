#ifndef PROCFS_H
#define PROCFS_H

#include <stdint.h>
#include "vfs.h"

// Read callback: generates up to 'size' bytes at offset 'off' into 'buf'
typedef int (*procfs_read_fn)(uint32_t off, uint32_t size, char *buf);

// Command callback: invoked when writing to /proc/cmd/<name>
typedef void (*procfs_cmd_fn)(char *args);

// Initialise procfs and register default files
void         procfs_init        (void);

// Set total memory (called by mntfs during init)
void         procfs_set_meminfo  (uint32_t mem_total_kb);

// Return the procfs root VFS node (to be mounted)
vfs_node_t  *procfs_get_root(void);

// Register a read-only virtual file under /proc
int procfs_register_file(const char *name, procfs_read_fn read_fn);

// Register a read/write command node under /proc/cmd
int procfs_register_cmd (const char *name,
                          procfs_read_fn read_fn,
                          procfs_cmd_fn  cmd_fn);

// Store/read binary module snapshots under /proc/mdls
int procfs_register_blob  (const char *name, const void *data, uint32_t size);
int procfs_unregister_blob(const char *name);

// Unregister a virtual file or command node
int procfs_unregister_file(const char *name);
int procfs_unregister_cmd (const char *name);

#endif