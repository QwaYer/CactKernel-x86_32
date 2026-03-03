#ifndef PROCFS_H
#define PROCFS_H

#include "vfs.h"


//Public api
void             procfs_init(void);
struct vfs_node* procfs_get_root(void);
struct vfs_node* procfs_get_commands_dir(void);


int              procfs_register_command(const char* name, const char* help,
                                         void (*handler)(char* args));


struct vfs_node* procfs_find_command(const char* name);
void             procfs_exec_command(struct vfs_node* node, char* args);

#endif