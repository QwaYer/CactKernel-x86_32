#ifndef PIPE_H
#define PIPE_H

#include <stdint.h>
#include "vfs.h"
#include "sync.h"

#define PIPE_BUF_SIZE   4096        
#define O_NONBLOCK      0x0001     

#define PIPE_MAGIC      0x50495045                                 

typedef struct pipe {
    uint32_t    magic;                  // PIPE_MAGIC                        
    uint8_t     buf[PIPE_BUF_SIZE];    // кольцевой буфер данных             
    uint32_t    read_pos;              // позиция чтения                     
    uint32_t    write_pos;             // позиция записи                     
    uint32_t    len;                   // количество байт в буфере           
    int         flags;                 // O_NONBLOCK и прочие                

    int         write_open;            // >0 пока есть хотя бы один writer   
    int         read_open;             // >0 пока есть хотя бы один reader   

    mutex_t     lock;                  // защита полей                       

    char*       name;                  
} pipe_t;

typedef struct pipe_end {
    pipe_t*  pipe;
    int      is_write;  
} pipe_end_t;


//Public api
int pipe_create(struct vfs_node* pipefd[2], int flags);

struct vfs_node* fifo_create(const char* name, int flags);

int pipe_read (pipe_t* p, unsigned int offset, unsigned int size, char* buffer);
int pipe_write(pipe_t* p, unsigned int offset, unsigned int size, char* buffer);

void pipe_close_read (pipe_t* p);
void pipe_close_write(pipe_t* p);
void pipe_destroy(pipe_t* p);

#endif 