#include "shell.h"
#include "kernel.h"
#include "libc.h"

void sh_reboot(char* args) {
    kprint("\nRebooting...");
    unsigned char good = 0x02;
    while (good & 0x02) good = port_byte_in(0x64);
    port_byte_out(0x64, 0xFE);
}

void sh_help(char* args) {
    kprint("\nLuxOS Shell v0.1\n");
    kprint("Available commands: help, clear, fetch, ls, reboot\n");
}

void sh_ls(char* args) {
    kprint("\nContents of /:\n");
    fat16_list_root();
}

void sh_fetch(char* args) {
    kprint("\n");
    kprint("  _                ___  ____  \n");
    kprint(" | |   _   ___  __/ _ \\/ ___| \n");
    kprint(" | |  | | | \\ \\/ / | | \\___ \\ \n");
    kprint(" | |__| |_| |>  <| |_| |___) |\n");
    kprint(" |_____\\__,_/_/\\_\\\\___/|____/ \n");
    kprint("\n OS: LuxOS 0.0.8\n");
    kprint(" Kernel: x86_32 Standard\n");
}

struct shell_command sh_tab[] = {
    {"help",    "Show help",    sh_help},
    {"ls",      "List files",   sh_ls},
    {"fetch",   "System info",  sh_fetch},
    {"clear",   "Clear screen", clear_screen},
    {"reboot",  "Restart",      sh_reboot}
};

#define SH_TAB_COUNT (sizeof(sh_tab) / sizeof(struct shell_command))

void shell_execute(char* input) {
    if (input[0] == '\0') return;

    for (int i = 0; i < SH_TAB_COUNT; i++) {
        if (compare_string(input, sh_tab[i].name) == 0) {
            sh_tab[i].handler(input);
            return;
        }
    }
    kprint("\nUnknown command: ");
    kprint(input);
}