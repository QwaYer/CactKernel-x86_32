#include "commands.h"
#include "mntfs.h"
#include "etcfs.h"
#include "procfs.h"
#include "shell.h"
#include "task.h"
#include "kernel.h"
#include "keyboard.h"
#include "vfs.h"
#include "ext4.h"
#include "memory.h"
#include "libc.h"
#include "net.h"
#include "ip.h"
#include "icmp.h"
#include "udp.h"
#include "fb.h"

/* ── Утилиты ──────────────────────────────────────────────────── */

static char* _skip_token(char* s) {
    if (!s) return 0;
    while (*s && *s != ' ') s++;
    while (*s == ' ') s++;
    return (*s == '\0') ? 0 : s;
}

static void _trim(char* s) {
    if (!s) return;
    for (int i = 0; s[i]; i++)
        if (s[i] == ' ' || s[i] == '\n' || s[i] == '\r') { s[i] = '\0'; break; }
}

static uint32_t _parse_ip(char* s) {
    uint32_t ip = 0;
    for (int i = 0; i < 4; i++) {
        uint32_t val = 0;
        while (*s >= '0' && *s <= '9') { val = val*10 + (*s-'0'); s++; }
        ip = (ip << 8) | (val & 0xFF);
        if (*s == '.') s++;
    }
    return ip;
}

static unsigned char _bcd_to_bin(unsigned char b) {
    return ((b >> 4) * 10) + (b & 0x0F);
}

/* ── Навигация ────────────────────────────────────────────────── */

static void cmd_cd(char* args) {
    char* name = _skip_token(args);
    if (!name || compare_string(name, "/") == 0) { shell_resetdir(); return; }
    if (compare_string(name, "..") == 0) { shell_popdir(); return; }

    struct vfs_node* next = finddir_vfs(current_dir, name);
    if (!next || next->type != VFS_DIRECTORY) {
        kprint("\nDirectory not found: "); kprint(name); kprint("\n"); return;
    }
    shell_pushdir(current_dir);
    current_dir = next;
    if (compare_string(current_path, "/") != 0) {
        int len = strlen(current_path);
        current_path[len] = '/'; current_path[len+1] = '\0';
    }
    strcat(current_path, name);
}

static void cmd_pwd(char* args) {
    (void)args;
    kprint("\n"); kprint(current_path); kprint("\n");
}

/* ── Файловая система ─────────────────────────────────────────── */

static void cmd_ls(char* args) {
    (void)args;
    kprint("\n");
    if (!current_dir || current_dir == vfs_root) {
        kprint_color("Mounted disks (/):\n", COLOR_LIGHT_CYAN);
        listdir_vfs(vfs_root);
        kprint("\nTip: cd system  then  ls\n");
    } else {
        kprint_color("Directory listing:\n", COLOR_LIGHT_MAGENTA);
        listdir_vfs(current_dir);
    }
}

static void cmd_mkdir(char* args) {
    char* name = _skip_token(args);
    if (!name) { kprint("\nUsage: mkdir <n>\n"); return; }
    if (mkdir_vfs(current_dir ? current_dir : vfs_root, name) == 0)
        kprint("\nDirectory created.\n");
    else kprint("\nError: could not create directory.\n");
}

static void cmd_rmdir(char* args) {
    char* name = _skip_token(args);
    if (!name) { kprint("\nUsage: rmdir <dir>\n"); return; }
    if (rmdir_vfs(current_dir ? current_dir : vfs_root, name) == 0)
        kprint("\nDirectory removed.\n");
    else kprint("\nError: not found or not empty.\n");
}

static void cmd_tch(char* args) {
    char* name = _skip_token(args);
    if (!name) { kprint("\nUsage: tch <file>\n"); return; }
    if (create_vfs(current_dir ? current_dir : vfs_root, name) == 0)
        kprint("\nFile created.\n");
    else kprint("\nError: could not create file.\n");
}

static void cmd_rm(char* args) {
    char* name = _skip_token(args);
    if (!name) { kprint("\nUsage: rm <file>\n"); return; }
    if (delete_vfs(current_dir ? current_dir : vfs_root, name) == 0)
        kprint("\nFile deleted.\n");
    else kprint("\nError: file not found.\n");
}

static void cmd_cat(char* args) {
    char* name = _skip_token(args);
    if (!name) { kprint("\nUsage: cat <file>\n"); return; }
    struct vfs_node* dir  = current_dir ? current_dir : vfs_root;
    struct vfs_node* node = finddir_vfs(dir, name);
    if (!node) { kprint("\nError: not found.\n"); return; }
    if (node->type == VFS_DIRECTORY) { kprint("\nError: is a directory.\n"); return; }
    unsigned int sz = node->size ? node->size : 64*1024;
    char* buf = (char*)kmalloc(sz + 1);
    if (!buf) { kprint("\nOut of memory.\n"); return; }
    memory_set(buf, 0, sz + 1);
    int bytes = read_vfs(node, 0, sz, buf);
    if (bytes <= 0) { kfree_heap(buf); kprint("\n(empty)\n"); return; }
    buf[bytes] = '\0';
    kprint("\n"); kprint(buf); kprint("\n");
    kfree_heap(buf);
}

static void cmd_wrt(char* args) {
    char* name = _skip_token(args);
    if (!name) { kprint("\nUsage: wrt <file> <text>\n"); return; }
    char* text = _skip_token(name);
    if (!text) { kprint("\nUsage: wrt <file> <text>\n"); return; }
    struct vfs_node* node = finddir_vfs(current_dir ? current_dir : vfs_root, name);
    if (!node) { kprint("\nError: not found.\n"); return; }
    if (write_vfs(node, 0, strlen(text), text) > 0) kprint("\nWritten.\n");
    else kprint("\nError: write failed.\n");
}

static void cmd_echo(char* args) {
    char* text = _skip_token(args);
    kprint("\n"); if (text) kprint(text); kprint("\n");
}

static void cmd_mount(char* args) {
    char* devname = _skip_token(args);
    if (!devname) { mntfs_list(); return; }

    char* mntname = _skip_token(devname);
    if (!mntname) {
        kprint("\nUsage: mount <device> <n>\n");
        kprint("  Devices : hda  hdb  hdc  hdd\n");
        kprint("  Example : mount hdb data\n");
        kprint("  Result  : /system/mnt/data  (saved to fstab)\n");
        return;
    }

    _trim(devname);
    _trim(mntname);

    char fullname[MNTFS_NAME_LEN];
    int fi = 0;
    const char* pfx = "system/mnt/";
    for (int i = 0; pfx[i] && fi < MNTFS_NAME_LEN-1; i++) fullname[fi++] = pfx[i];
    for (int i = 0; mntname[i] && fi < MNTFS_NAME_LEN-1; i++) fullname[fi++] = mntname[i];
    fullname[fi] = '\0';

    if (mntfs_get(fullname)) {
        kprint("\nError: '"); kprint(mntname);
        kprint("' already mounted at /system/mnt/"); kprint(mntname);
        kprint("\nUse umount "); kprint(mntname); kprint(" first.\n");
        return;
    }

    uint16_t base; uint8_t slave;
    if (mntfs_resolve_device(devname, &base, &slave) < 0) {
        kprint("\nError: unknown device '"); kprint(devname);
        kprint("'\nKnown: hda  hdb  hdc  hdd\n");
        return;
    }

    kprint("\nProbing "); kprint(devname); kprint("...\n");
    struct vfs_node* root = ext4_mount_disk(base, slave);
    if (!root) {
        kprint("Error: no ext4 on "); kprint(devname); kprint("\n");
        return;
    }
    strncpy(root->name, mntname, 128);

    if (mntfs_mount(fullname, devname, root, 1) < 0) {
        kprint("Error: mount table full\n"); return;
    }

    kprint("Mounted "); kprint(devname);
    kprint(" -> /system/mnt/"); kprint(mntname);
    kprint("  [saved to fstab]\n");
}

static void cmd_umount(char* args) {
    char* name = _skip_token(args);
    if (!name) {
        kprint("\nUsage: umount <n>\n");
        kprint("  Example: umount data   (unmounts /system/mnt/data)\n");
        return;
    }
    _trim(name);

    char fullname[MNTFS_NAME_LEN];

    if (mntfs_get(name)) {
        strncpy(fullname, name, MNTFS_NAME_LEN);
    } else {
        int fi = 0;
        const char* pfx = "system/mnt/";
        for (int i = 0; pfx[i] && fi < MNTFS_NAME_LEN-1; i++) fullname[fi++] = pfx[i];
        for (int i = 0; name[i]  && fi < MNTFS_NAME_LEN-1; i++) fullname[fi++] = name[i];
        fullname[fi] = '\0';

        if (!mntfs_get(fullname)) {
            fi = 0;
            const char* pfx2 = "system/";
            for (int i = 0; pfx2[i] && fi < MNTFS_NAME_LEN-1; i++) fullname[fi++] = pfx2[i];
            for (int i = 0; name[i]  && fi < MNTFS_NAME_LEN-1; i++) fullname[fi++] = name[i];
            fullname[fi] = '\0';
        }
    }

    const char* protected[] = {
        "system", "system/etc", "system/dev",
        "system/proc", "system/mnt", 0
    };
    for (int pi = 0; protected[pi]; pi++) {
        if (strncmp(fullname, protected[pi]) == 0) {
            kprint("\nError: '"); kprint(fullname);
            kprint("' is a system mount point and cannot be unmounted.\n");
            return;
        }
    }

    if (!mntfs_get(fullname)) {
        kprint("\nError: '"); kprint(name); kprint("' is not mounted.\n");
        kprint("Use 'mount' to see mounted disks.\n");
        return;
    }

    struct vfs_node* mp = mntfs_get(fullname);
    if (mp && mp == current_dir) {
        kprint("\nError: cannot umount current directory. Run cd / first.\n");
        return;
    }

    if (mntfs_umount(fullname) < 0) {
        kprint("\nError: umount failed.\n");
        return;
    }
    kprint("\nUnmounted /"); kprint(fullname);
    kprint("  [removed from fstab]\n");
}

/* ── Системные команды ────────────────────────────────────────── */

static void cmd_help(char* args) {
    (void)args;
    kprint("\n");
    kprint_color("Lux Shell commands:\n", COLOR_LIGHT_CYAN);
    kprint("  Navigation : cd  pwd\n");
    kprint("  Files      : ls  mkdir  rmdir  tch  rm  cat  wrt  echo\n");
    kprint("  Disks      : mount  umount\n");
    kprint("  System     : help  fetch  clear  reboot  free  date  uptime  ps  kill  run\n");
    kprint("  Network    : ipconfig  ping  udptest\n");
    kprint("  Debug      : kbd  pic\n");
    kprint("\n");
    kprint_color("Disk layout:\n", COLOR_LIGHT_BROWN);
    kprint("  /system          <- hda (auto-mounted)\n");
    kprint("  /system/etc      <- etcfs (configs, fstab)\n");
    kprint("  /system/dev      <- devfs\n");
    kprint("  /system/proc     <- procfs\n");
    kprint("  /system/mnt/...  <- user disks (mount hdb data)\n");
}

static void cmd_fetch(char* args) {
    (void)args;
    kprint("\n");
    kprint_color("  _                 \n", COLOR_LIGHT_CYAN);
    kprint_color(" | |   _   ___  __  \n", COLOR_LIGHT_CYAN);
    kprint_color(" | |  | | | \\ \\/ /  \n", COLOR_LIGHT_CYAN);
    kprint_color(" | |__| |_| |>  <   \n", COLOR_LIGHT_CYAN);
    kprint_color(" |_____\\__,_/_/\\_\\  \n", COLOR_LIGHT_CYAN);
    kprint("\n");
    kprint_color(" Kernel:  ", COLOR_LIGHT_BROWN); kprint("Lux Kernel 0.1.0\n");
    kprint_color(" Arch:    ", COLOR_LIGHT_BROWN); kprint("x86_32\n");
    kprint_color(" Shell:   ", COLOR_LIGHT_BROWN); kprint("v0.5\n");
    char buf[32];
    unsigned int free_mem = get_free_heap_memory();
    kprint_color(" Memory:  ", COLOR_LIGHT_BROWN);
    itoa(free_mem, buf); kprint(buf); kprint(" bytes free\n");
    kprint("\n");
    if (fb_get_width() > 0) {
        uint32_t sx = get_cursor_x()+8, sy = get_cursor_y(), sq = 16;
        uint32_t colors[] = {
            COLOR_BLACK, COLOR_RED, COLOR_GREEN, COLOR_BROWN,
            COLOR_BLUE, COLOR_MAGENTA, COLOR_CYAN, COLOR_LIGHT_GREY,
            COLOR_DARK_GREY, COLOR_LIGHT_RED, COLOR_LIGHT_GREEN, COLOR_LIGHT_BROWN,
            COLOR_LIGHT_BLUE, COLOR_LIGHT_MAGENTA, COLOR_LIGHT_CYAN, COLOR_WHITE
        };
        for (int i = 0; i < 8; i++) fb_fill_rect(sx+i*sq, sy,    sq, sq, colors[i]);
        for (int i = 0; i < 8; i++) fb_fill_rect(sx+i*sq, sy+sq, sq, sq, colors[i+8]);
        kprint("\n\n\n");
    }
}

static void cmd_clear(char* args)  { (void)args; clear_screen(); }

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
    kprint("\nFree heap: ");
    itoa(get_free_heap_memory(), buf); kprint(buf); kprint(" bytes\n");
}

static void cmd_date(char* args) {
    (void)args;
    port_byte_out(0x70,0x00); unsigned char sec  =_bcd_to_bin(port_byte_in(0x71));
    port_byte_out(0x70,0x02); unsigned char min  =_bcd_to_bin(port_byte_in(0x71));
    port_byte_out(0x70,0x04); unsigned char hour =_bcd_to_bin(port_byte_in(0x71));
    port_byte_out(0x70,0x07); unsigned char day  =_bcd_to_bin(port_byte_in(0x71));
    port_byte_out(0x70,0x08); unsigned char mon  =_bcd_to_bin(port_byte_in(0x71));
    port_byte_out(0x70,0x09); unsigned char year =_bcd_to_bin(port_byte_in(0x71));
    char buf[16];
    kprint("\nDate: ");
    itoa(day,buf);  kprint(buf); kprint(".");
    itoa(mon,buf);  kprint(buf); kprint(".20");
    itoa(year,buf); kprint(buf); kprint(" ");
    itoa(hour,buf); kprint(buf); kprint(":");
    if (min<10) kprint("0"); itoa(min,buf); kprint(buf); kprint(":");
    if (sec<10) kprint("0"); itoa(sec,buf); kprint(buf); kprint("\n");
}

static void cmd_uptime(char* args) { (void)args; kprint("\nSystem running.\n"); }

static void cmd_ps(char* args)  { (void)args; list_tasks(); }

static void cmd_kill(char* args) {
    char* p = _skip_token(args);
    if (!p) { kprint("\nUsage: kill <pid>\n"); return; }
    uint32_t pid = 0;
    for (; *p>='0'&&*p<='9'; p++) pid=pid*10+(*p-'0');
    if (!pid) { kprint("\nInvalid PID\n"); return; }
    task_kill(pid);
    kprint("\nSignal sent.\n");
}

static void cmd_run(char* args) {
    char* name = _skip_token(args);
    if (!name) { kprint("\nUsage: run <file>\n"); return; }
    if (create_elf_task(name)) kprint("\nTask created.\n");
    else kprint("\nError: could not load ELF.\n");
}

/* ── Отладка ──────────────────────────────────────────────────── */

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
    kprint("\nPIC Master (0x21): 0x");
    hex_to_ascii(port_byte_in(0x21), buf); kprint(buf);
    kprint("\nPIC Slave  (0xA1): 0x");
    hex_to_ascii(port_byte_in(0xA1), buf); kprint(buf);
    kprint("\n");
}

/* ── Сеть ─────────────────────────────────────────────────────── */

static void cmd_ipconfig(char* args) {
    (void)args;
    kprint("\nNetwork:\n  IP:  ");
    char buf[16];
    itoa((MY_IP>>24)&0xFF,buf); kprint(buf); kprint(".");
    itoa((MY_IP>>16)&0xFF,buf); kprint(buf); kprint(".");
    itoa((MY_IP>> 8)&0xFF,buf); kprint(buf); kprint(".");
    itoa( MY_IP     &0xFF,buf); kprint(buf); kprint("\n  MAC: ");
    for (int i=0;i<6;i++) {
        uint8_t b=my_mac.b[i];
        buf[0]="0123456789ABCDEF"[b>>4];
        buf[1]="0123456789ABCDEF"[b&0xF];
        buf[2]=0; kprint(buf);
        if (i<5) kprint(":");
    }
    kprint("\n");
}

static void cmd_ping(char* args) {
    char* ip = _skip_token(args);
    if (!ip) { kprint("\nUsage: ping <ip>\n"); return; }
    kprint("\nPinging "); kprint(ip); kprint("...\n");
    icmp_send_echo_request(htonl(_parse_ip(ip)), 1234, 1);
}

static void cmd_udptest(char* args) {
    char* ip   = _skip_token(args);  if (!ip)   { kprint("\nUsage: udptest <ip> <port> <msg>\n"); return; }
    char* port = _skip_token(ip);    if (!port)  { kprint("\nUsage: udptest <ip> <port> <msg>\n"); return; }
    char* msg  = _skip_token(port);  if (!msg)   msg = "Hello from Lux!";
    uint16_t p = 0;
    for (char* c=port; *c>='0'&&*c<='9'; c++) p=p*10+(*c-'0');
    skb_t* skb = skb_alloc(); if (!skb) return;
    uint8_t* data = skb_put(skb, strlen(msg));
    for (int i=0;i<(int)strlen(msg);i++) data[i]=msg[i];
    kprint("\nSending UDP to "); kprint(ip); kprint(":"); kprint(port); kprint("\n");
    udp_output(skb, htonl(_parse_ip(ip)), 12345, p);
}

void commands_init(void) {
    procfs_register_command("cd",       "Change directory",               cmd_cd);
    procfs_register_command("pwd",      "Print working directory",        cmd_pwd);
    procfs_register_command("ls",       "List directory",                 cmd_ls);
    procfs_register_command("mkdir",    "Create directory",               cmd_mkdir);
    procfs_register_command("rmdir",    "Remove directory",               cmd_rmdir);
    procfs_register_command("tch",      "Create file",                    cmd_tch);
    procfs_register_command("rm",       "Delete file",                    cmd_rm);
    procfs_register_command("cat",      "Print file contents",            cmd_cat);
    procfs_register_command("wrt",      "Write text to file",             cmd_wrt);
    procfs_register_command("echo",     "Print text",                     cmd_echo);
    procfs_register_command("mount",    "Mount disk (mount hdb data)",    cmd_mount);
    procfs_register_command("umount",   "Unmount disk (umount data)",     cmd_umount);
    procfs_register_command("help",     "Show help",                      cmd_help);
    procfs_register_command("fetch",    "System info",                    cmd_fetch);
    procfs_register_command("clear",    "Clear screen",                   cmd_clear);
    procfs_register_command("reboot",   "Restart system",                 cmd_reboot);
    procfs_register_command("free",     "Memory info",                    cmd_free);
    procfs_register_command("date",     "Show date/time",                 cmd_date);
    procfs_register_command("uptime",   "System uptime",                  cmd_uptime);
    procfs_register_command("ps",       "Task list",                      cmd_ps);
    procfs_register_command("kill",     "Kill process by PID",            cmd_kill);
    procfs_register_command("run",      "Run ELF binary",                 cmd_run);
    procfs_register_command("kbd",      "Keyboard stats",                 cmd_kbd);
    procfs_register_command("pic",      "PIC masks",                      cmd_pic);
    procfs_register_command("ipconfig", "Network configuration",          cmd_ipconfig);
    procfs_register_command("ping",     "Send ICMP echo request",         cmd_ping);
    procfs_register_command("udptest",  "Send UDP packet",                cmd_udptest);
}