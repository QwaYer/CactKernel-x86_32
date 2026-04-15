#ifndef SHM_H
#define SHM_H

#include <stdint.h>

#define TASK_SHM_MAX  16

#define IPC_CREAT     0x0200
#define IPC_EXCL      0x0400
#define IPC_PRIVATE   0

#define IPC_RMID      0
#define IPC_STAT      2

#define SHM_RDONLY    0x1000
#define SHM_RND       0x2000

typedef struct {
    int      shm_id;
    uint32_t shm_vaddr;
} shm_attachment_t;

typedef shm_attachment_t task_shm_attach_t;

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
