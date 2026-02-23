#include "shell.h"
#include "procfs.h"
#include "vfs.h"
#include "kernel.h"
#include "memory.h"
#include "libc.h"


#define DIR_STACK_MAX 32

static struct vfs_node* dir_stack[DIR_STACK_MAX];
static int              dir_stack_top = 0;

struct vfs_node* current_dir       = 0;
char             current_path[512] = "/";

void shell_pushdir(struct vfs_node* node) {
    if (dir_stack_top < DIR_STACK_MAX - 1)
        dir_stack[dir_stack_top++] = node;
}

void shell_popdir(void) {
    if (dir_stack_top > 0) {
        dir_stack_top--;
        current_dir = dir_stack[dir_stack_top]
                      ? dir_stack[dir_stack_top]
                      : vfs_root;
        int len = strlen(current_path);
        while (len > 1 && current_path[len - 1] != '/') len--;
        if (len > 1) len--;
        current_path[len] = '\0';
        if (len == 0) copy_string(current_path, "/");
    } else {
        current_dir = vfs_root;
        copy_string(current_path, "/");
    }
}

void shell_resetdir(void) {
    dir_stack_top = 0;
    dir_stack[0]  = 0;
    current_dir   = vfs_root;
    copy_string(current_path, "/");
}


void shell_execute(char* input) {
    if (!input || input[0] == '\0') return;

    if (!current_dir) shell_resetdir();

    char cmd_name[64];
    int i = 0;
    while (input[i] && input[i] != ' ' && i < 63) {
        cmd_name[i] = input[i];
        i++;
    }
    cmd_name[i] = '\0';

    struct vfs_node* node = procfs_find_command(cmd_name);
    if (node) {
        procfs_exec_command(node, input);
        return;
    }

    kprint("\nUnknown command: ");
    kprint(cmd_name);
    kprint("\nType 'help' for a list of commands.\n");
}