#ifndef SHM_H
#define SHM_H

#include <stdint.h>

#define SHM_MAX_SEGMENTS   32
#define SHM_MAX_ATTACH     8
#define SHM_MAX_PAGES      64

/* shmget flags */
#define IPC_CREAT    0x0200
#define IPC_EXCL     0x0400
#define IPC_PRIVATE  0

/* shmctl commands */
#define IPC_STAT     1
#define IPC_SET      2
#define IPC_RMID     3

/* shmat flags */
#define SHM_RDONLY   0x1000

typedef struct {
    uint32_t pid;
    uint32_t vaddr;
} shm_attach_t;

typedef struct {
    int          id;
    int          key;
    uint32_t     size;
    uint32_t     npages;
    uint32_t     phys_pages[SHM_MAX_PAGES];
    shm_attach_t attachments[SHM_MAX_ATTACH];
    int          nattach;
    int          marked_destroy;    /* IPC_RMID was called */
    int          used;
    uint32_t     creator_pid;
} shm_segment_t;

typedef struct {
    int      shm_id;
    uint32_t shm_vaddr;
} task_shm_attach_t;

#define TASK_SHM_MAX  8

/* public api */
void shm_init(void);
int  shm_get(int key, uint32_t size, int flags);
uint32_t shm_at(int shmid, uint32_t shmaddr, int flags);
int  shm_dt(uint32_t shmaddr);
int  shm_ctl(int shmid, int cmd, void* buf);

/* called from task cleanup */
void shm_detach_all(uint32_t pid, uint32_t* pd);

void shm_debug_dump(void);

#endif