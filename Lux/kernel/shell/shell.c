#include "shell.h"
#include "kernel.h"
#include "keyboard.h"
#include "vfs.h"
#include "memory.h"
#include "libc.h"
#include "net.h"
#include "ip.h"
#include "icmp.h"
#include "udp.h"


struct vfs_node* current_dir = 0;
char current_path[256] = "/";

static char* skip_token(char* s) {
    if (!s) return 0;
    while (*s && *s != ' ') s++;   
    if (*s == ' ') { *s = '\0'; s++; }
    while (*s == ' ') s++;        
    return (*s == '\0') ? 0 : s;
}

void sh_help(char* args) {
    kprint("\n");
    kprint_color("LuxOS Shell v0.2 (Enhanced)\n", COLOR_LIGHT_CYAN);
    kprint_color("Available commands:\n", COLOR_LIGHT_GREY);
    kprint("  help, clear, fetch, ls, ps, reboot, uptime, free, date\n");
    kprint("  cd <dir>, pwd, mkdir <dir>, rmdir <dir>\n");
    kprint("  cat <file>, wrt <file> <text>\n");
    kprint("  tch <file>, rm <file>, echo <text>\n");
    kprint("  ipconfig, ping <ip>, udptest <ip> <port> <msg>\n");
}

void sh_ls(char* args) {
    if (!current_dir) current_dir = vfs_root;
    kprint("\n");
    kprint_color("Contents of directory:\n", COLOR_LIGHT_MAGENTA);
    listdir_vfs(current_dir);
}

void sh_cd(char* args) {
    char* name = skip_token(args);
    if (!current_dir) current_dir = vfs_root;

    if (!name || compare_string(name, "/") == 0) {
        current_dir = vfs_root;
        return;
    }

    if (compare_string(name, "..") == 0) {
        /* Для простоты пока возвращаемся в корень,
           так как у vfs_node нет ссылки на parent */
        current_dir = vfs_root;
        return;
    }

    struct vfs_node* next = finddir_vfs(current_dir, name);
    if (next && next->type == VFS_DIRECTORY) {
        current_dir = next;
        /* Обновляем путь (упрощенно) */
        if (compare_string(current_path, "/") != 0) {
            int len = strlen(current_path);
            current_path[len] = '/';
            current_path[len+1] = '\0';
            strcat(current_path, name);
        } else {
            strcat(current_path, name);
        }
    } else {
        kprint("\nDirectory not found.\n");
    }
}

void sh_pwd(char* args) {
    kprint("\n");
    kprint(current_path);
    kprint("\n");
}

void sh_mkdir(char* args) {
    char* name = skip_token(args);
    if (!name) { kprint("\nUsage: mkdir <name>\n"); return; }
    if (!current_dir) current_dir = vfs_root;

    if (mkdir_vfs(current_dir, name) == 0)
        kprint("\nDirectory created.\n");
    else
        kprint("\nError: could not create directory.\n");
}

void sh_free(char* args) {
    char buf[32];
    unsigned int free_mem = get_free_heap_memory();
    kprint("\nFree heap memory: ");
    itoa(free_mem, buf); kprint(buf);
    kprint(" bytes\n");
}

static unsigned char bcd_to_bin(unsigned char bcd) {
    return ((bcd >> 4) * 10) + (bcd & 0x0F);
}

void sh_date(char* args) {
    port_byte_out(0x70, 0x00); unsigned char sec = bcd_to_bin(port_byte_in(0x71));
    port_byte_out(0x70, 0x02); unsigned char min = bcd_to_bin(port_byte_in(0x71));
    port_byte_out(0x70, 0x04); unsigned char hour = bcd_to_bin(port_byte_in(0x71));
    port_byte_out(0x70, 0x07); unsigned char day = bcd_to_bin(port_byte_in(0x71));
    port_byte_out(0x70, 0x08); unsigned char month = bcd_to_bin(port_byte_in(0x71));
    port_byte_out(0x70, 0x09); unsigned char year = bcd_to_bin(port_byte_in(0x71));

    char buf[16];
    kprint("\nDate: ");
    itoa(day, buf); kprint(buf); kprint(".");
    itoa(month, buf); kprint(buf); kprint(".20");
    itoa(year, buf); kprint(buf); kprint(" ");
    itoa(hour, buf); kprint(buf); kprint(":");
    if (min < 10) kprint("0");
    itoa(min, buf); kprint(buf); kprint(":");
    if (sec < 10) kprint("0");
    itoa(sec, buf); kprint(buf); kprint("\n");
}

void sh_uptime(char* args) {
    /* заглушка */
    kprint("\nSystem is up and running.\n");
}

static uint32_t parse_ip(char* s) {
    uint32_t ip = 0;
    for (int i = 0; i < 4; i++) {
        uint32_t val = 0;
        while (*s >= '0' && *s <= '9') {
            val = val * 10 + (*s - '0');
            s++;
        }
        ip = (ip << 8) | (val & 0xFF);
        if (*s == '.') s++;
    }
    return ip;
}

void sh_ipconfig(char* args) {
    kprint("\nNetwork Configuration:\n");
    kprint("  IP Address:  ");
    char buf[16];
    itoa((MY_IP >> 24) & 0xFF, buf); kprint(buf); kprint(".");
    itoa((MY_IP >> 16) & 0xFF, buf); kprint(buf); kprint(".");
    itoa((MY_IP >> 8) & 0xFF, buf); kprint(buf); kprint(".");
    itoa(MY_IP & 0xFF, buf); kprint(buf); kprint("\n");

    kprint("  MAC Address: ");
    for (int i = 0; i < 6; i++) {
        uint8_t byte = my_mac.b[i];
        buf[0] = "0123456789ABCDEF"[byte >> 4];
        buf[1] = "0123456789ABCDEF"[byte & 0xF];
        buf[2] = 0;
        kprint(buf);
        if (i < 5) kprint(":");
    }
    kprint("\n");
}

void sh_ping(char* args) {
    char* ip_str = skip_token(args);
    if (!ip_str) {
        kprint("\nUsage: ping <ip>\n");
        return;
    }

    uint32_t dst_ip = parse_ip(ip_str);
    kprint("\nPinging ");
    kprint(ip_str);
    kprint("...\n");

    icmp_send_echo_request(htonl(dst_ip), 1234, 1);
}

void sh_udp_test(char* args) {
    char* ip_str = skip_token(args);
    if (!ip_str) {
        kprint("\nUsage: udptest <ip> <port> <message>\n");
        return;
    }
    char* port_str = skip_token(ip_str);
    if (!port_str) {
        kprint("\nUsage: udptest <ip> <port> <message>\n");
        return;
    }
    char* msg = skip_token(port_str);
    if (!msg) msg = "Hello from LuxOS!";

    uint32_t dst_ip = parse_ip(ip_str);
    uint16_t port = 0;
    char* p = port_str;
    while (*p >= '0' && *p <= '9') {
        port = port * 10 + (*p - '0');
        p++;
    }

    skb_t* skb = skb_alloc();
    if (!skb) return;
    uint8_t* data = skb_put(skb, strlen(msg));
    for (int i = 0; i < (int)strlen(msg); i++) data[i] = msg[i];

    kprint("\nSending UDP to ");
    kprint(ip_str);
    kprint(":");
    kprint(port_str);
    kprint("\n");

    udp_output(skb, htonl(dst_ip), 12345, port);
}

void sh_fetch(char* args) {
    kprint("\n");
    kprint_color("  _                ___  ____  \n", COLOR_LIGHT_CYAN);
    kprint_color(" | |   _   ___  __/ _ \\/ ___| \n", COLOR_LIGHT_CYAN);
    kprint_color(" | |  | | | \\ \\/ / | | \\___ \\ \n", COLOR_LIGHT_CYAN);
    kprint_color(" | |__| |_| |>  <| |_| |___) |\n", COLOR_LIGHT_CYAN);
    kprint_color(" |_____\\__,_/_/\\_\\\\___/|____/ \n", COLOR_LIGHT_CYAN);
    kprint("\n");
    kprint_color(" OS: ", COLOR_LIGHT_BROWN); kprint("LuxOS 0.0.8\n");
    kprint_color(" Kernel: ", COLOR_LIGHT_BROWN); kprint("x86_32 Standard\n");
    kprint_color(" Shell: ", COLOR_LIGHT_BROWN); kprint("v0.2 (Enhanced)\n");
    
    char buf[32];
    unsigned int free_mem = get_free_heap_memory();
    kprint_color(" Memory: ", COLOR_LIGHT_BROWN); itoa(free_mem, buf); kprint(buf); kprint(" bytes free\n");
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

void sh_rmdir(char* args) {
    char* name = skip_token(args);
    if (!name) { kprint("\nUsage: rmdir <dir>\n"); return; }
    if (delete_vfs(vfs_root, name) == 0)
        kprint("\nDirectory deleted.\n");
    else
        kprint("\nError: directory not found.\n");
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


void sh_run(char* args) {
    char* name = skip_token(args);
    if (!name) { kprint("\nUsage: run <file>\n"); return; }

    if (create_elf_task(name)) {
        kprint("\nTask created and scheduled.\n");
    } else {
        kprint("\nError: could not load ELF.\n");
    }
}

static struct shell_command sh_tab[] = {
    {"help",   "Show help",      sh_help},
    {"run",    "Run ELF file",   sh_run},
    {"ls",     "List files",     sh_ls},
    {"cd",     "Change dir",     sh_cd},
    {"fetch",  "System info",    sh_fetch},
    {"clear",  "Clear screen",   (void(*)(char*))clear_screen},
    {"reboot", "Restart",        sh_reboot},
    {"ps",     "Task list",      sh_list_tasks},
    {"kbd",    "Keyboard stats", sh_kbd},
    {"pic",    "PIC masks",      sh_pic},
    {"echo",   "Print text",     sh_echo},
    {"tch",    "Create file",    sh_tch},
    {"mkdir",  "Create dir",     sh_mkdir},
    {"rmdir",  "Delete dir",     sh_rmdir},
    {"rm",     "Delete file",    sh_rm},
    {"cat",    "Print file",     sh_cat},
    {"wrt",    "Write to file",  sh_wrt},
    {"pwd",    "Show path",      sh_pwd},
    {"free",   "Memory info",    sh_free},
    {"date",   "Show date",      sh_date},
    {"uptime", "System uptime",  sh_uptime},
    {"ipconfig", "Network info", sh_ipconfig},
    {"ping",   "Send ICMP echo", sh_ping},
    {"udptest", "Send UDP packet", sh_udp_test},
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