#ifndef SHELL_H
#define SHELL_H

#include "vfs.h"

extern vfs_node_t* current_dir;
extern char             current_path[512];

extern vfs_node_t* shell_out;
extern vfs_node_t* shell_stdin;
extern vfs_node_t* current_ext4_dir;
void shell_recalc_ext4_dir(void);  
extern char        current_mnt_path[128];

void shell_write(const char* s);

char* shell_read_stdin(int* out_len);

void shell_init(void);
void shell_pushdir(vfs_node_t* node);
void shell_popdir(void);
void shell_resetdir(void);
void shell_execute(char* input);

#endif