#include "commands.h"
#include "procfs.h"
#include "shell.h"
#include "task.h"
#include "kernel.h"
#include "keyboard.h"
#include "vfs.h"
#include "memory.h"
#include "libc.h"
#include "net.h"
#include "ip.h"
#include "icmp.h"
#include "udp.h"


//Утилиты
static char* _skip_token(char* s) {
    if (!s) return 0;
    while (*s && *s != ' ') s++;
    while (*s == ' ') s++;
    return (*s == '\0') ? 0 : s;
}

static uint32_t _parse_ip(char* s) {
    uint32_t ip = 0;
    for (int i = 0; i < 4; i++) {
        uint32_t val = 0;
        while (*s >= '0' && *s <= '9') { val = val * 10 + (*s - '0'); s++; }
        ip = (ip << 8) | (val & 0xFF);
        if (*s == '.') s++;
    }
    return ip;
}

static unsigned char _bcd_to_bin(unsigned char bcd) {
    return ((bcd >> 4) * 10) + (bcd & 0x0F);
}


//Навигация
static void cmd_cd(char* args) {
    char* name = _skip_token(args);

    if (!name || compare_string(name, "/") == 0) {
        shell_resetdir();
        return;
    }

    if (compare_string(name, "..") == 0) {
        shell_popdir();
        return;
    }

    struct vfs_node* next = finddir_vfs(current_dir, name);
    if (!next || next->type != VFS_DIRECTORY) {
        kprint("\nDirectory not found: ");
        kprint(name);
        kprint("\n");
        return;
    }

    shell_pushdir(current_dir);
    current_dir = next;

    if (compare_string(current_path, "/") != 0) {
        int len = strlen(current_path);
        current_path[len]     = '/';
        current_path[len + 1] = '\0';
    }
    strcat(current_path, name);
}

static void cmd_pwd(char* args) {
    (void)args;
    kprint("\n");
    kprint(current_path);
    kprint("\n");
}


//Файловая система
static void cmd_ls(char* args) {
    (void)args;
    if (!current_dir) current_dir = vfs_root;
    kprint("\n");
    kprint_color("Directory listing:\n", COLOR_LIGHT_MAGENTA);
    listdir_vfs(current_dir);
}

static void cmd_mkdir(char* args) {
    char* name = _skip_token(args);
    if (!name) { kprint("\nUsage: mkdir <name>\n"); return; }
    struct vfs_node* dir = current_dir ? current_dir : vfs_root;
    if (mkdir_vfs(dir, name) == 0) kprint("\nDirectory created.\n");
    else kprint("\nError: could not create directory.\n");
}

static void cmd_rmdir(char* args) {
    char* name = _skip_token(args);
    if (!name) { kprint("\nUsage: rmdir <dir>\n"); return; }
    struct vfs_node* dir = current_dir ? current_dir : vfs_root;
    if (rmdir_vfs(dir, name) == 0) kprint("\nDirectory removed.\n");
    else kprint("\nError: directory not found or not empty.\n");
}

static void cmd_tch(char* args) {
    char* name = _skip_token(args);
    if (!name) { kprint("\nUsage: tch <file>\n"); return; }
    struct vfs_node* dir = current_dir ? current_dir : vfs_root;
    if (create_vfs(dir, name) == 0) kprint("\nFile created.\n");
    else kprint("\nError: could not create file.\n");
}

static void cmd_rm(char* args) {
    char* name = _skip_token(args);
    if (!name) { kprint("\nUsage: rm <file>\n"); return; }
    struct vfs_node* dir = current_dir ? current_dir : vfs_root;
    if (delete_vfs(dir, name) == 0) kprint("\nFile deleted.\n");
    else kprint("\nError: file not found.\n");
}

static void cmd_cat(char* args) {
    char* name = _skip_token(args);
    if (!name) { kprint("\nUsage: cat <file>\n"); return; }
    struct vfs_node* dir  = current_dir ? current_dir : vfs_root;
    struct vfs_node* node = finddir_vfs(dir, name);
    if (!node) { kprint("\nError: file not found.\n"); return; }
    if (node->type == VFS_DIRECTORY) { kprint("\nError: is a directory.\n"); return; }
    unsigned int read_size = node->size ? node->size : (64 * 1024);
    char* buf = (char*)kmalloc(read_size + 1);
    if (!buf) { kprint("\nError: out of memory.\n"); return; }
    memory_set(buf, 0, read_size + 1);
    int bytes = read_vfs(node, 0, read_size, buf);
    if (bytes <= 0) { kfree_heap(buf); kprint("\n(empty file)\n"); return; }
    buf[bytes] = '\0';
    kprint("\n"); kprint(buf); kprint("\n");
    kfree_heap(buf);
}

static void cmd_wrt(char* args) {
    char* name = _skip_token(args);
    if (!name) { kprint("\nUsage: wrt <file> <text>\n"); return; }
    char* text = _skip_token(name);
    if (!text) { kprint("\nUsage: wrt <file> <text>\n"); return; }
    struct vfs_node* dir  = current_dir ? current_dir : vfs_root;
    struct vfs_node* node = finddir_vfs(dir, name);
    if (!node) { kprint("\nError: file not found.\n"); return; }
    int n = write_vfs(node, 0, strlen(text), text);
    if (n > 0) kprint("\nWritten.\n");
    else kprint("\nError: write failed.\n");
}

static void cmd_echo(char* args) {
    char* text = _skip_token(args);
    kprint("\n");
    if (text) kprint(text);
    kprint("\n");
}


//Системные команды
static void cmd_help(char* args) {
    (void)args;
    kprint("\n");
    kprint_color("Lux Shell commands from /proc/commands/\n", COLOR_LIGHT_CYAN);
    kprint("Use 'ls' in /proc/commands to see all commands.\n");
}

static void cmd_fetch(char* args) {
    (void)args;
    kprint("\n");
    kprint_color("  _                ___  ____  \n", COLOR_LIGHT_CYAN);
    kprint_color(" | |   _   ___  __/ _ \\/ ___| \n", COLOR_LIGHT_CYAN);
    kprint_color(" | |  | | | \\ \\/ / | | \\___ \\ \n", COLOR_LIGHT_CYAN);
    kprint_color(" | |__| |_| |>  <| |_| |___) |\n", COLOR_LIGHT_CYAN);
    kprint_color(" |_____\\__,_/_/\\_\\\\___/|____/ \n", COLOR_LIGHT_CYAN);
    kprint("\n");
    kprint_color(" Kernel:  ", COLOR_LIGHT_BROWN); kprint("Lux Kernel 0.1.0\n");
    kprint_color(" Arch:    ", COLOR_LIGHT_BROWN); kprint("x86_32\n");
    kprint_color(" Shell:   ", COLOR_LIGHT_BROWN); kprint("v0.4\n");
    char buf[32];
    unsigned int free_mem = get_free_heap_memory();
    kprint_color(" Memory:  ", COLOR_LIGHT_BROWN);
    itoa(free_mem, buf); kprint(buf); kprint(" bytes free\n");
}

static void cmd_clear(char* args) {
    (void)args;
    clear_screen();
}

static void cmd_reboot(char* args) {
    (void)args;
    kprint("\nRebooting...\n");
    unsigned char good = 0x02;
    while (good & 0x02) good = port_byte_in(0x64);
    port_byte_out(0x64, 0xFE);
}

static void cmd_free(char* args) {
    (void)args;
    char buf[32];
    unsigned int free_mem = get_free_heap_memory();
    kprint("\nFree heap memory: ");
    itoa(free_mem, buf); kprint(buf);
    kprint(" bytes\n");
}

static void cmd_date(char* args) {
    (void)args;
    port_byte_out(0x70, 0x00); unsigned char sec   = _bcd_to_bin(port_byte_in(0x71));
    port_byte_out(0x70, 0x02); unsigned char min   = _bcd_to_bin(port_byte_in(0x71));
    port_byte_out(0x70, 0x04); unsigned char hour  = _bcd_to_bin(port_byte_in(0x71));
    port_byte_out(0x70, 0x07); unsigned char day   = _bcd_to_bin(port_byte_in(0x71));
    port_byte_out(0x70, 0x08); unsigned char month = _bcd_to_bin(port_byte_in(0x71));
    port_byte_out(0x70, 0x09); unsigned char year  = _bcd_to_bin(port_byte_in(0x71));
    char buf[16];
    kprint("\nDate: ");
    itoa(day, buf);   kprint(buf); kprint(".");
    itoa(month, buf); kprint(buf); kprint(".20");
    itoa(year, buf);  kprint(buf); kprint(" ");
    itoa(hour, buf);  kprint(buf); kprint(":");
    if (min < 10) kprint("0");
    itoa(min, buf);   kprint(buf); kprint(":");
    if (sec < 10) kprint("0");
    itoa(sec, buf);   kprint(buf); kprint("\n");
}

static void cmd_uptime(char* args) {
    (void)args;
    kprint("\nSystem is up and running.\n");
}

static void cmd_ps(char* args) {
    (void)args;
    list_tasks();
}

static void cmd_kill(char* args) {
    char* pid_str = _skip_token(args);
    if (!pid_str) { kprint("\nUsage: kill <pid>\n"); return; }
    uint32_t pid = 0;
    for (char* p = pid_str; *p >= '0' && *p <= '9'; p++)
        pid = pid * 10 + (*p - '0');
    if (!pid) { kprint("\nError: invalid PID\n"); return; }
    task_kill(pid);
    kprint("\nSignal sent to PID "); kprint(pid_str); kprint("\n");
}

static void cmd_run(char* args) {
    char* name = _skip_token(args);
    if (!name) { kprint("\nUsage: run <file>\n"); return; }
    if (create_elf_task(name)) kprint("\nTask created.\n");
    else kprint("\nError: could not load ELF.\n");
}


//Отладка
static void cmd_kbd(char* args) {
    (void)args;
    char buf[32];
    kprint("\nKeyboard IRQ count: ");
    itoa(keyboard_irq_count, buf); kprint(buf);
    kprint("\nLast scancode: 0x");
    hex_to_ascii(last_scancode_raw, buf); kprint(buf);
    kprint("\nLast char: ");
    char tmp[2] = { last_char ? last_char : '?', 0 };
    kprint(tmp); kprint("\n");
}

static void cmd_pic(char* args) {
    (void)args;
    char buf[32];
    kprint("\nPIC Master mask (0x21): 0x");
    hex_to_ascii(port_byte_in(0x21), buf); kprint(buf);
    kprint("\nPIC Slave  mask (0xA1): 0x");
    hex_to_ascii(port_byte_in(0xA1), buf); kprint(buf);
    kprint("\n");
}


// Сеть
static void cmd_ipconfig(char* args) {
    (void)args;
    kprint("\nNetwork Configuration:\n");
    kprint("  IP:  ");
    char buf[16];
    itoa((MY_IP >> 24) & 0xFF, buf); kprint(buf); kprint(".");
    itoa((MY_IP >> 16) & 0xFF, buf); kprint(buf); kprint(".");
    itoa((MY_IP >>  8) & 0xFF, buf); kprint(buf); kprint(".");
    itoa( MY_IP        & 0xFF, buf); kprint(buf); kprint("\n");
    kprint("  MAC: ");
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

static void cmd_ping(char* args) {
    char* ip_str = _skip_token(args);
    if (!ip_str) { kprint("\nUsage: ping <ip>\n"); return; }
    uint32_t dst = _parse_ip(ip_str);
    kprint("\nPinging "); kprint(ip_str); kprint("...\n");
    icmp_send_echo_request(htonl(dst), 1234, 1);
}

static void cmd_udptest(char* args) {
    char* ip_str   = _skip_token(args);
    if (!ip_str)   { kprint("\nUsage: udptest <ip> <port> <msg>\n"); return; }
    char* port_str = _skip_token(ip_str);
    if (!port_str) { kprint("\nUsage: udptest <ip> <port> <msg>\n"); return; }
    char* msg      = _skip_token(port_str);
    if (!msg) msg  = "Hello from Lux!";
    uint32_t dst = _parse_ip(ip_str);
    uint16_t port = 0;
    for (char* p = port_str; *p >= '0' && *p <= '9'; p++)
        port = port * 10 + (*p - '0');
    skb_t* skb = skb_alloc();
    if (!skb) return;
    uint8_t* data = skb_put(skb, strlen(msg));
    for (int i = 0; i < (int)strlen(msg); i++) data[i] = msg[i];
    kprint("\nSending UDP to "); kprint(ip_str); kprint(":"); kprint(port_str); kprint("\n");
    udp_output(skb, htonl(dst), 12345, port);
}

//Регистрация
void commands_init(void) {
    /* навигация */
    procfs_register_command("cd",      "Change directory",        cmd_cd);
    procfs_register_command("pwd",     "Print working directory", cmd_pwd);
    /* файловая система */
    procfs_register_command("ls",      "List directory",          cmd_ls);
    procfs_register_command("mkdir",   "Create directory",        cmd_mkdir);
    procfs_register_command("rmdir",   "Remove directory",        cmd_rmdir);
    procfs_register_command("tch",     "Create file",             cmd_tch);
    procfs_register_command("rm",      "Delete file",             cmd_rm);
    procfs_register_command("cat",     "Print file contents",     cmd_cat);
    procfs_register_command("wrt",     "Write text to file",      cmd_wrt);
    procfs_register_command("echo",    "Print text",              cmd_echo);
    /* система */
    procfs_register_command("help",    "Show help",               cmd_help);
    procfs_register_command("fetch",   "System info",             cmd_fetch);
    procfs_register_command("clear",   "Clear screen",            cmd_clear);
    procfs_register_command("reboot",  "Restart system",          cmd_reboot);
    procfs_register_command("free",    "Memory info",             cmd_free);
    procfs_register_command("date",    "Show date/time",          cmd_date);
    procfs_register_command("uptime",  "System uptime",           cmd_uptime);
    procfs_register_command("ps",      "Task list",               cmd_ps);
    procfs_register_command("kill",    "Kill process by PID",     cmd_kill);
    procfs_register_command("run",     "Run ELF binary",          cmd_run);
    /* отладка */
    procfs_register_command("kbd",     "Keyboard stats",          cmd_kbd);
    procfs_register_command("pic",     "PIC masks",               cmd_pic);
    /* сеть */
    procfs_register_command("ipconfig","Network configuration",   cmd_ipconfig);
    procfs_register_command("ping",    "Send ICMP echo request",  cmd_ping);
    procfs_register_command("udptest", "Send UDP packet",         cmd_udptest);
}