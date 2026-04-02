#ifndef _SHM_H
#define _SHM_H

#include <stdint.h>

#define IPC_CREAT    0x0200
#define IPC_EXCL     0x0400
#define IPC_PRIVATE  0

#define IPC_STAT     1
#define IPC_SET      2
#define IPC_RMID     3

#define SHM_RDONLY   0x1000

int      shmget(int key, unsigned int size, int flags);
void*    shmat(int shmid, const void* shmaddr, int flags);
int      shmdt(const void* shmaddr);
int      shmctl(int shmid, int cmd, void* buf);

#endif