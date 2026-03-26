#ifndef _SYS_STAT_H
#define _SYS_STAT_H

#include <stdint.h>

#define S_IFMT   0xF000
#define S_IFREG  0x8000
#define S_IFDIR  0x4000
#define S_IFCHR  0x2000
#define S_IFBLK  0x6000
#define S_IFIFO  0x1000

#define S_ISREG(m)  (((m) & S_IFMT) == S_IFREG)
#define S_ISDIR(m)  (((m) & S_IFMT) == S_IFDIR)
#define S_ISCHR(m)  (((m) & S_IFMT) == S_IFCHR)
#define S_ISBLK(m)  (((m) & S_IFMT) == S_IFBLK)
#define S_ISFIFO(m) (((m) & S_IFMT) == S_IFIFO)

struct stat {
    uint32_t st_ino;
    uint32_t st_mode;
    uint32_t st_size;
    uint32_t st_type;
};

int stat(const char *path, struct stat *buf);
int fstat(int fd, struct stat *buf);

#endif