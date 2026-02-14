#include "shell.h"
#include "kernel.h"
#include "keyboard.h"
#include "vfs.h"
#include "libc.h"


static char* skip_token(char* s) {
    if (!s) return 0;
    while (*s && *s != ' ') s++;   
    if (*s == ' ') { *s = '\0'; s++; }
    while (*s == ' ') s++;        
    return (*s == '\0') ? 0 : s;
}

void sh_help(char* args) {
    kprint("\nLuxOS Shell v0.2\n");
    kprint("Commands: help, clear, fetch, ls, ps, reboot\n");
    kprint("          cat <file>, wrt <file> <text>\n");
    kprint("          tch <file>, rm <file>, echo <text>\n");
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

void sh_reboot(char* args) {
    kprint("\nRebooting...");
    unsigned char good = 0x02;
    while (good & 0x02) good = port_byte_in(0x64);
    port_byte_out(0x64, 0xFE);
}

void sh_list_tasks(char* args) {
    list_tasks();
}

void sh_kbd(char* args) {
    char buf[32];
    kprint("\nKeyboard IRQ count: ");
    itoa(keyboard_irq_count, buf); kprint(buf);
    kprint("\nLast scancode (raw): 0x");
    hex_to_ascii(last_scancode_raw, buf); kprint(buf);
    kprint("\nLast char: ");
    char tmp[2] = { last_char ? last_char : '?', 0 };
    kprint(tmp);
    kprint("\n");
}

void sh_pic(char* args) {
    char buf[32];
    kprint("\nPIC Master mask (port 0x21): 0x");
    hex_to_ascii(port_byte_in(0x21), buf); kprint(buf);
    kprint("\nPIC Slave  mask (port 0xA1): 0x");
    hex_to_ascii(port_byte_in(0xA1), buf); kprint(buf);
    kprint("\n");
}

void sh_echo(char* args) {
    char* text = skip_token(args);
    kprint("\n");
    if (text) kprint(text);
    kprint("\n");
}

void sh_tch(char* args) {
    char* name = skip_token(args);
    if (!name) { kprint("\nUsage: tch <file>\n"); return; }

    int res = create_vfs(vfs_root, name);

    if (res == 0)
        kprint("\nFile created.\n");
    else
        kprint("\nError: could not create file.\n");
}

void sh_rm(char* args) {
    char* name = skip_token(args);
    if (!name) { kprint("\nUsage: rm <file>\n"); return; }
    if (delete_vfs(vfs_root, name) == 0)
        kprint("\nFile deleted.\n");
    else
        kprint("\nError: file not found.\n");
}

void sh_cat(char* args) {
    char* name = skip_token(args);
    if (!name) { kprint("\nUsage: cat <file>\n"); return; }

    struct vfs_node* node = finddir_vfs(vfs_root, name);
    if (!node) { kprint("\nError: file not found.\n"); return; }
    if (node->size == 0) { kprint("\n(empty file)\n"); return; }

    char* buf = (char*)kmalloc(node->size + 1);
    if (!buf) { kprint("\nError: out of memory.\n"); return; }

    int bytes = read_vfs(node, 0, node->size, buf);
    buf[bytes] = '\0';
    kprint("\n");
    kprint(buf);
    kprint("\n");
    kfree_heap(buf);
}

void sh_wrt(char* args) {
    char* name = skip_token(args);
    if (!name) { kprint("\nUsage: wrt <file> <text>\n"); return; }
    char* text = skip_token(name);
    if (!text) { kprint("\nUsage: wrt <file> <text>\n"); return; }

    struct vfs_node* node = finddir_vfs(vfs_root, name);
    if (!node) { kprint("\nError: file not found. Use tch first.\n"); return; }

    write_vfs(node, 0, strlen(text), text);
    kprint("\nWritten.\n");
}


static struct shell_command sh_tab[] = {
    {"help",   "Show help",      sh_help},
    {"ls",     "List files",     sh_ls},
    {"fetch",  "System info",    sh_fetch},
    {"clear",  "Clear screen",   (void(*)(char*))clear_screen},
    {"reboot", "Restart",        sh_reboot},
    {"ps",     "Task list",      sh_list_tasks},
    {"kbd",    "Keyboard stats", sh_kbd},
    {"pic",    "PIC masks",      sh_pic},
    {"echo",   "Print text",     sh_echo},
    {"tch",  "Create file",      sh_tch},
    {"rm",     "Delete file",    sh_rm},
    {"cat",    "Print file",     sh_cat},
    {"wrt",  "Write to file",    sh_wrt},
};

#define SH_TAB_COUNT (sizeof(sh_tab) / sizeof(struct shell_command))

void shell_execute(char* input) {
    if (!input || input[0] == '\0') return;

    int i = 0;
    while (input[i] && input[i] != ' ') i++;
    char saved = input[i];
    input[i] = '\0';

    for (int j = 0; j < (int)SH_TAB_COUNT; j++) {
        if (compare_string(input, sh_tab[j].name) == 0) {
            input[i] = saved;
            sh_tab[j].handler(input);
            return;
        }
    }

    input[i] = saved;
    kprint("\nUnknown command: ");
    kprint(input);
    kprint("\n");
}