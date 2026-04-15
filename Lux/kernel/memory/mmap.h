#ifndef MMAP_H
#define MMAP_H

#include <stdint.h>
#include "memory.h"

#define PROT_NONE   0x0
#define PROT_READ   0x1
#define PROT_WRITE  0x2
#define PROT_EXEC   0x4

#define MAP_SHARED      0x01
#define MAP_PRIVATE     0x02
#define MAP_FIXED       0x10
#define MAP_ANON        0x20
#define MAP_ANONYMOUS   MAP_ANON

#define MAP_FAILED      ((void*)-1)

#define MMAP_BASE       0x40000000u
#define MMAP_LIMIT      0xBF000000u

#define MMAP_MAX_REGIONS  256

typedef struct mmap_region {
    uint32_t  base;
    uint32_t  length;
    uint32_t  flags;
    uint32_t  prot;
    int       fd;
    uint32_t  file_off;
    uint8_t   is_used;
} mmap_region_t;

typedef struct mmap_table {
    mmap_region_t regions[MMAP_MAX_REGIONS];
    uint32_t      next_base;
} mmap_table_t;

void           mmap_table_init(mmap_table_t* tbl);
void           mmap_table_clone(mmap_table_t* src, mmap_table_t* dst,
                                uint32_t* src_pd, uint32_t* dst_pd);
void           mmap_table_free(mmap_table_t* tbl, uint32_t* pd);
void*          do_mmap(uint32_t* pd, mmap_table_t* tbl,
                       uint32_t hint, uint32_t length,
                       int prot, int flags, int fd, uint32_t offset);
int            do_munmap(uint32_t* pd, mmap_table_t* tbl,
                         uint32_t addr, uint32_t length);
int            do_mprotect(uint32_t* pd, mmap_table_t* tbl,
                           uint32_t addr, uint32_t length, int prot);
int            mmap_handle_fault(uint32_t* pd, mmap_table_t* tbl, uint32_t fault_addr);
mmap_region_t* mmap_find_region(mmap_table_t* tbl, uint32_t addr);
void           mmap_print_regions(const mmap_table_t* tbl);

#define SYS_MMAP      90
#define SYS_MUNMAP    91
#define SYS_MPROTECT  125

typedef struct {
    uint32_t addr;
    uint32_t length;
    int      prot;
    int      flags;
    int      fd;
    uint32_t offset;
} mmap_args_t;

#endif
