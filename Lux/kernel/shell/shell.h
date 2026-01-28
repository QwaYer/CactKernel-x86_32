#ifndef SHELL_H
#define SHELL_H

struct shell_command {
    char* name;
    char* help;
    void (*handler)(char* args);
};

void sh_help(char* args);
void sh_ls(char* args);
void sh_fetch(char* args);
void sh_reboot(char* args);

void shell_execute(char* input);

#endif