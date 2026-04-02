#ifndef SHM_H
#define SHM_H

#include <stdint.h>

/* Maximum shared-memory segments a single task may have attached at once. */
#define TASK_SHM_MAX  16

/* IPC flags (passed to shm_get) */
#define IPC_CREAT     0x0200
#define IPC_EXCL      0x0400
#define IPC_PRIVATE   0

/* shm_ctl commands */
#define IPC_RMID      0
#define IPC_STAT      2

/* shmat flags */
#define SHM_RDONLY    0x1000
#define SHM_RND       0x2000

/* Per-task attachment record stored in task_struct */
typedef struct {
    int      shm_id;     /* 0 = unused slot */
    uint32_t shm_vaddr;  /* virtual address where segment is mapped */
} shm_attachment_t;

/* Alias used by user's task.h commit */
typedef shm_attachment_t task_shm_attach_t;

/* Subset of shmid_ds returned by IPC_STAT */
typedef struct {
    uint32_t shm_segsz;
    uint32_t shm_cpid;
    uint32_t shm_lpid;
    uint32_t shm_nattch;
} shminfo_t;

int      shm_get(int key, uint32_t size, int flags);
uint32_t shm_at(int shmid, uint32_t shmaddr, int flags);
int      shm_dt(uint32_t shmaddr);
int      shm_ctl(int shmid, int cmd, void* buf);
void     shm_detach_all(uint32_t pid, uint32_t* page_directory);

#endif
