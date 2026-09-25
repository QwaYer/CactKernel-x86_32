#ifndef PROCFS_INTERNAL_H
#define PROCFS_INTERNAL_H

#include "procfs.h"

// A read-only virtual file (e.g. cpuinfo, meminfo)
typedef struct proc_file {
    char            name[64];
    procfs_read_fn  read_fn;      // generates file content on demand
    vfs_node_t      node;
    struct proc_file *next;
} proc_file_t;

/* procfs.c — core state shared with the standard-file modules. */
extern proc_file_t *file_list;
extern vfs_node_t   procfs_root;
extern vfs_node_t   proc_self_dir;

/* procfs_proc.c — per-process /proc service nodes (self/, <pid>/). */
void procfs_proc_init(void);

/* procfs_proc.c — the Linux-shaped numeric process directories.
 * procfs_proc_pid_dir() maps a directory name ("1", "42", ...) to the node
 * for that live pid, or NULL when no such task exists. */
vfs_node_t *procfs_proc_pid_dir(const char *name);

/* k-th live pid in task-list order (index for readdir/listdir).  Returns 0 on
 * success, -1 past the end. */
int         procfs_proc_pid_at (uint32_t index, uint32_t *pid_out);

/* procfs_std.c — default /proc file generators. */
int _cpuinfo_read(uint32_t off, uint32_t size, char *buf);
int _apic_read(uint32_t off, uint32_t size, char *buf);
int _meminfo_read(uint32_t off, uint32_t size, char *buf);
int _uptime_read(uint32_t off, uint32_t size, char *buf);
int _version_read(uint32_t off, uint32_t size, char *buf);
int _time_read(uint32_t off, uint32_t size, char *buf);
int _wallclock_read(uint32_t off, uint32_t size, char *buf);
int _uname_read(uint32_t off, uint32_t size, char *buf);
int _mounts_read(uint32_t off, uint32_t size, char *buf);
int _modules_read(uint32_t off, uint32_t size, char *buf);
int _usb_read(uint32_t off, uint32_t size, char *buf);

#endif
