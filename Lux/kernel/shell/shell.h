#ifndef SHELL_H
#define SHELL_H

#include "vfs.h"

extern struct vfs_node* current_dir;
extern char             current_path[512];

extern struct vfs_node* shell_out;

extern struct vfs_node* shell_stdin;

void shell_write(const char* s);

char* shell_read_stdin(int* out_len);

void shell_init(void);
void shell_pushdir(struct vfs_node* node);
void shell_popdir(void);
void shell_resetdir(void);
void shell_execute(char* input);

#endif