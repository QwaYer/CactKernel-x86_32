#ifndef SHELL_H
#define SHELL_H

#include "vfs.h"

extern struct vfs_node* current_dir;
extern char             current_path[512];

void shell_init(void);     
void shell_pushdir(struct vfs_node* node);
void shell_popdir(void);
void shell_resetdir(void);
void shell_execute(char* input);

#endif