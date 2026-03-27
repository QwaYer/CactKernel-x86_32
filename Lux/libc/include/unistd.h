#ifndef UNISTD_H
#define UNISTD_H

#include <stddef.h>

typedef int pid_t;
typedef int ssize_t;
typedef int off_t;

#define STDIN_FILENO  0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2

#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

ssize_t read(int fd, void *buf, size_t count);
ssize_t write(int fd, const void *buf, size_t count);
int close(int fd);
pid_t fork(void);
int execve(const char *pathname, char *const argv[], char *const envp[]);
pid_t getpid(void);
pid_t getppid(void);
off_t lseek(int fd, off_t offset, int whence);
pid_t waitpid(pid_t pid, int *status, int options);
unsigned int sleep(unsigned int seconds);
int usleep(unsigned int usec);
void *sbrk(int increment);
int brk(void *addr);
char *getcwd(char *buf, int size);
int chdir(const char *path);
int ioctl(int fd, unsigned long cmd, void *arg);
int mkdir(const char *pathname, int mode);
int rmdir(const char *pathname);
int pipe(int pipefd[2]);
int dup2(int oldfd, int newfd);

#endif