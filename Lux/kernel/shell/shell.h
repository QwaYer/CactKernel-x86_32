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
void sh_kbd(char* args);
void sh_pic(char* args);
void sh_cat(char* args);
void sh_wrt(char* args);
void sh_tch(char* args);
void sh_rm(char* args);
void sh_echo(char* args);

void shell_execute(char* input);

#endif