#ifndef PROCFS_INTERNAL_H
#define PROCFS_INTERNAL_H

#include "procfs.h"

#define MDLS_MAX_FILES  32

// A read-only virtual file (e.g. cpuinfo, meminfo)
typedef struct proc_file {
    char            name[64];
    procfs_read_fn  read_fn;      // generates file content on demand
    vfs_node_t      node;
    struct proc_file *next;
} proc_file_t;

/* procfs.c — core state shared with the mdls/ and standard-file modules. */
extern proc_file_t *file_list;
extern vfs_node_t   procfs_root;
extern vfs_node_t   mdls_dir;
extern vfs_ops_t    mdls_dir_ops;

/* procfs_mdls.c */
void procfs_mdls_init(void);

/* procfs_std.c — default /proc file generators. */
int _cpuinfo_read(uint32_t off, uint32_t size, char *buf);
int _apic_read(uint32_t off, uint32_t size, char *buf);
int _meminfo_read(uint32_t off, uint32_t size, char *buf);
int _uptime_read(uint32_t off, uint32_t size, char *buf);
int _version_read(uint32_t off, uint32_t size, char *buf);
int _tasks_read(uint32_t off, uint32_t size, char *buf);

#endif
