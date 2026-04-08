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
#include "elf.h"   
#include "dynlink.h"      
#include "proc_mm.h"  
#include "pagecache.h"
#include "oom.h"


//Утилиты 
static vfs_node_t* _resolve_path(const char *path) {
    if (!path) return 0;
    vfs_node_t *base = (path[0] == '/') ? vfs_root : (current_dir ? current_dir : vfs_root);
    return vfs_walk_path(base, path);
}

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

static void _copy_cwd_to_task(struct task_struct* t) {
    if (!t) return;
    int i = 0;
    while (current_path[i] && i < 255) { t->cwd[i] = current_path[i]; i++; }
    t->cwd[i] = '\0';
}

// Навигация 
static void cmd_cd(char* args) {
    char* path = _skip_token(args);
    if (!path || compare_string(path, "/") == 0) { shell_resetdir(); return; }
    if (compare_string(path, "..") == 0) { shell_popdir(); return; }

    vfs_node_t* start = current_dir ? current_dir : vfs_root;
    if (path[0] == '/') { start = vfs_root; path++; }
    else {
        char _fseg[32]; int _fi = 0;
        while (path[_fi] && path[_fi] != '/' && _fi < 31) { _fseg[_fi] = path[_fi]; _fi++; }
        _fseg[_fi] = '\0';
        if (mntfs_get_ext4(_fseg) || finddir_vfs(vfs_root, _fseg))
            start = vfs_root;
    }

    vfs_node_t* saved_dir = current_dir;
    char saved_path[512];
    copy_string(saved_path, current_path);

    vfs_node_t* cur = start;
    char tmp_path[512];
    copy_string(tmp_path, current_path);

    char seg[128];
    const char *p = path;
    int pushed = 0;
    while (*p) {
        int si = 0;
        while (*p && *p != '/' && si < 127) seg[si++] = *p++;
        seg[si] = '\0';
        if (*p == '/') p++;
        if (si == 0) continue;

        vfs_node_t* next = finddir_vfs(cur, seg);
        if (!next || next->type != VFS_DIRECTORY) {
            for (int i = 0; i < pushed; i++) shell_popdir();
            current_dir = saved_dir;
            copy_string(current_path, saved_path);
            kprint("\nDirectory not found: "); kprint(seg); kprint("\n");
            return;
        }

        shell_pushdir(cur);
        if (compare_string(tmp_path, "/") != 0) {
            int len = strlen(tmp_path);
            tmp_path[len] = '/'; tmp_path[len+1] = '\0';
        }
        strcat(tmp_path, seg);
        copy_string(current_path, tmp_path);
        current_dir = next;
        cur = next;
        pushed++;
    }

    shell_recalc_ext4_dir();
}


static void cmd_pwd(char* args) {
    (void)args;
    kprint("\n"); kprint(current_path); kprint("\n");
}


static void cmd_ls(char* args) {
    (void)args;
    kprint("\n");
    if (!current_dir || current_dir == vfs_root) {
        kprint_color("Mounted disks (/):\n", COLOR_LIGHT_CYAN);
        listdir_vfs(vfs_root);
        kprint("\nTip: cd hda/raw  or  cd hda/sys\n");
    } else {
        kprint_color("Directory listing:\n", COLOR_LIGHT_MAGENTA);
        listdir_vfs(current_dir);
    }
}

static void cmd_lsraw(char* args) {
    (void)args;
    kprint("\n");

    vfs_node_t* dir = current_ext4_dir;
    if (!dir) {
        kprint("Not in a raw/ directory. Try: cd hda/raw\n");
        return;
    }

    if (!dir->ops || !dir->ops->readdir) {
        kprint("(no readdir)\n"); return;
    }

    kprint_color("Raw ext4 [", COLOR_LIGHT_BROWN);
    kprint_color(current_mnt_path[0] ? current_mnt_path : "?", COLOR_LIGHT_BROWN);
    kprint_color("]:\n", COLOR_LIGHT_BROWN);

    vfs_dirent_t* de; int any = 0;
    for (uint32_t i = 0; (de = dir->ops->readdir(dir, i)); i++) {
        kprint("  "); kprint(de->name); kprint("\n"); any = 1;
    }
    if (!any) kprint("  (empty)\n");
}


static void cmd_mkdir(char* args) {
    char* name = _skip_token(args);
    if (!name) { kprint("\nUsage: mkdir <n>\n"); return; }
    if (mkdir_vfs(current_dir ? current_dir : vfs_root, name) == 0) {
        pc_flush_dev(0);
        kprint("\nDirectory created.\n");
    }
    else kprint("\nError: could not create directory.\n");
}

static void cmd_rmdir(char* args) {
    char* name = _skip_token(args);
    if (!name) { kprint("\nUsage: rmdir <dir>\n"); return; }
    if (rmdir_vfs(current_dir ? current_dir : vfs_root, name) == 0) {
        pc_flush_dev(0);
        kprint("\nDirectory removed.\n");
    }
    else kprint("\nError: not found or not empty.\n");
}

static void cmd_tch(char* args) {
    char* name = _skip_token(args);
    if (!name) { kprint("\nUsage: tch <file>\n"); return; }
    if (create_vfs(current_dir ? current_dir : vfs_root, name) == 0) {
        pc_flush_dev(0);
        kprint("\nFile created.\n");
    }
    else kprint("\nError: could not create file.\n");
}

static void cmd_rm(char* args) {
    char* name = _skip_token(args);
    if (!name) { kprint("\nUsage: rm <file>\n"); return; }
    if (delete_vfs(current_dir ? current_dir : vfs_root, name) == 0) {
        pc_flush_dev(0);
        kprint("\nFile deleted.\n");
    }
    else kprint("\nError: file not found.\n");
}

static void cmd_cat(char* args) {
    char* name = _skip_token(args);
    if (!name) { kprint("\nUsage: cat <file> [bytes]\n"); return; }

    char* sz_arg = _skip_token(name);
    uint32_t max_sz = 0;
    if (sz_arg) {
        for (char* p = sz_arg; *p >= '0' && *p <= '9'; p++)
            max_sz = max_sz * 10 + (uint32_t)(*p - '0');
    }

    vfs_node_t* node = _resolve_path(name);
    if (!node) { kprint("\nError: not found.\n"); return; }
    if (node->type == VFS_DIRECTORY) { kprint("\nError: is a directory.\n"); return; }

    uint32_t sz;
    int is_chardev = (node->type == VFS_CHARDEVICE || node->type == VFS_PIPE);
    if (max_sz > 0) {
        sz = max_sz;
    } else if (is_chardev) {
        sz = 256;
    } else {
        sz = node->size ? node->size : 64 * 1024;
    }

    char* buf = (char*)kmalloc(sz + 1);
    if (!buf) { kprint("\nOut of memory.\n"); return; }
    memory_set(buf, 0, sz + 1);
    int bytes = read_vfs(node, 0, sz, buf);
    if (bytes <= 0) { kfree_heap(buf); shell_write("\n(empty)\n"); return; }
    buf[bytes] = '\0';
    shell_write("\n");
    shell_write(buf);
    shell_write("\n");
    kfree_heap(buf);
}

static void cmd_wrt(char* args) {
    char* name = _skip_token(args);
    if (!name) { kprint("\nUsage: wrt <file> [text]\n"); return; }

    vfs_node_t* node = _resolve_path(name);
    if (!node) { kprint("\nError: not found.\n"); return; }

    if (shell_stdin) {
        int len = 0;
        char* buf = shell_read_stdin(&len);
        if (buf && len > 0) {
            write_vfs(node, 0, (unsigned int)len, buf);
            kfree_heap(buf);
            pc_flush_dev(0);
            kprint("\nWritten.\n");
        } else {
            if (buf) kfree_heap(buf);
            kprint("\nError: stdin empty.\n");
        }
        return;
    }

    char* text = _skip_token(name);
    if (!text) { kprint("\nUsage: wrt <file> <text>\n"); return; }
    if (write_vfs(node, 0, strlen(text), text) > 0) {
        pc_flush_dev(0);
        kprint("\nWritten.\n");
    }
    else kprint("\nError: write failed.\n");
}

static void cmd_echo(char* args) {
    char* text = _skip_token(args);
    shell_write("\n");
    if (text) shell_write(text);
    shell_write("\n");
}

static void cmd_mount(char* args) {
    char* devname = _skip_token(args);
    if (!devname) { mntfs_list(); return; }
    _trim(devname);

    if (mntfs_get_ext4(devname)) {
        kprint("\nError: '"); kprint(devname); kprint("' already mounted.\n");
        return;
    }

    uint32_t dev;
    if (mntfs_resolve_device(devname, &dev) < 0) {
        kprint("\nError: unknown device '"); kprint(devname);
        kprint("'\nKnown: nvme0n1  nvme0n2  nvme1n1  nvme1n2\n");
        return;
    }

    kprint("\nProbing "); kprint(devname); kprint("...\n");
    vfs_node_t* root = ext4_mount_disk(dev);
    if (!root) { kprint("Error: no ext4 on "); kprint(devname); kprint("\n"); return; }

    if (mntfs_mount_disk(devname, root, 1) < 0) {
        kprint("Error: mount failed\n"); return;
    }
    kprint("Mounted /"); kprint(devname); kprint("/raw  [saved to mounts]\n");
}


static void cmd_umount(char* args) {
    char* devname = _skip_token(args);
    if (!devname) { kprint("\nUsage: umount <device>\n"); return; }
    _trim(devname);
    int r = mntfs_umount_disk(devname);
    if (r == 0)  { kprint("\nUnmounted /"); kprint(devname); kprint("/\n"); }
    else if (r == -2) kprint("\nError: cannot unmount boot disk (nvme0n1)\n");
    else { kprint("\nError: '"); kprint(devname); kprint("' not mounted\n"); }
}


static void cmd_help(char* args) {
    (void)args;
    kprint("\n");
    kprint_color("Lux Shell commands:\n", COLOR_LIGHT_CYAN);
    kprint("  Navigation : cd  pwd\n");
    kprint("  Files      : ls  lsraw  mkdir  rmdir  tch  rm  cat  wrt  echo\n");
    kprint("  Disks      : mount  umount\n");
    kprint("  System     : help  fetch  clear  reboot  free  date  uptime  ps  kill  run\n");
    kprint("  Network    : ipconfig  ping  udptest\n");
    kprint("  Debug      : kbd  pic\n");
    kprint("\n");
    kprint_color("Pipes & redirection:\n", COLOR_LIGHT_BROWN);
    kprint("  cmd1 | cmd2 | cmd3    pipeline\n");
    kprint("  cmd > file            redirect stdout (overwrite)\n");
    kprint("  cmd >> file           redirect stdout (append)\n");
    kprint("  cmd < file            redirect stdin from file\n");
    kprint("  cmd 2> file           redirect stderr to file\n");
    kprint("  cmd 2>&1              merge stderr into stdout\n");
    kprint("\n");
    kprint_color("run usage:\n", COLOR_LIGHT_BROWN);
    kprint("  run <path>            <- static or dynamic ELF\n");
    kprint("  run <path> --static   <- force static loader\n");
    kprint("\n");
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
    kprint_color(" Kernel:  ", COLOR_LIGHT_BROWN); kprint("Lux Kernel 0.8.8\n");
    kprint_color(" Arch:    ", COLOR_LIGHT_BROWN); kprint("x86_32\n");
    kprint_color(" Shell:   ", COLOR_LIGHT_BROWN); kprint("v0.8.8\n");
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
    kprint("\nSyncing disks...\n");
    pc_flush_dev(0);
    kprint("Rebooting...\n");
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

static void cmd_oom(char* args) {
    (void)args;
    oom_print_stats();
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
    char* path = _skip_token(args);
    if (!path) {
        kprint("\nUsage: run <path>\n");
        kprint("  <path>     : path to ELF binary (static or dynamic)\n");
        return;
    }

    int force_static = 0;
    char* flag = _skip_token(path);
    if (flag) {
        const char* fs = "--static";
        int match = 1;
        for (int i = 0; fs[i]; i++)
            if (flag[i] != fs[i]) { match = 0; break; }
        if (match) force_static = 1;
    }

    vfs_node_t *base = (path[0] == '/') ? vfs_root : (current_dir ? current_dir : vfs_root);
    vfs_node_t* file = vfs_walk_path(base, path);
    if (!file) {
        kprint("\nError: file not found: "); kprint(path); kprint("\n");
        return;
    }

    Elf32_Ehdr hdr;
    if (read_vfs(file, 0, sizeof(Elf32_Ehdr), (char*)&hdr) <= 0) {
        kprint("\nError: cannot read ELF header\n");
        return;
    }

    if (*(uint32_t*)hdr.e_ident != ELF_MAGIC) {
        kprint("\nError: not an ELF file\n");
        return;
    }
    if (hdr.e_machine != 3) {
        kprint("\nError: ELF is not i386\n");
        return;
    }
    if (hdr.e_type != 2 && hdr.e_type != 3) {
        kprint("\nError: ELF is not executable (ET_EXEC/ET_DYN)\n");
        return;
    }

    int has_dynamic = 0;
    if (!force_static) {
        for (int i = 0; i < hdr.e_phnum; i++) {
            Elf32_Phdr ph;
            if (read_vfs(file,
                         hdr.e_phoff + (uint32_t)i * hdr.e_phentsize,
                         sizeof(Elf32_Phdr),
                         (char*)&ph) <= 0)
                break;
            if (ph.p_type == PT_DYNAMIC) { has_dynamic = 1; break; }
        }
    }

    uint32_t* pd = vmm_create_address_space();
    if (!pd) {
        kprint("\nError: cannot allocate page directory\n");
        return;
    }

    proc_page_tracker_t* tracker =
        (proc_page_tracker_t*)kmalloc(sizeof(proc_page_tracker_t));
    if (!tracker) {
        kfree_page(pd);
        kprint("\nError: out of kernel memory\n");
        return;
    }
    proc_tracker_init(tracker);

    void* entry = 0;

    if (has_dynamic) {
        dyn_ctx_t* ctx = (dyn_ctx_t*)kmalloc(sizeof(dyn_ctx_t));
        if (!ctx) {
            kfree_heap(tracker);
            kfree_page(pd);
            kprint("\nError: out of kernel memory (dyn_ctx)\n");
            return;
        }

        kprint("\n[run] dynamic ELF detected, loading shared libraries...\n");
        char _full_path_dyn[256];
        if (path[0] == '/') {
            { int _i=0; while(path[_i]&&_i<255){_full_path_dyn[_i]=path[_i];_i++;} _full_path_dyn[_i]='\0'; }
        } else {
            int ci = 0;
            while (current_path[ci] && ci < 250) { _full_path_dyn[ci] = current_path[ci]; ci++; }
            if (ci > 0 && _full_path_dyn[ci-1] != '/') _full_path_dyn[ci++] = '/';
            for (int i = 0; path[i] && ci < 255; i++) _full_path_dyn[ci++] = path[i];
            _full_path_dyn[ci] = '\0';
        }
        entry = load_elf_dynamic(_full_path_dyn, pd, tracker, ctx);

        if (!entry) {
            dynlink_unload_all(ctx);
            kfree_heap(ctx);
            proc_free_pages(tracker);
            kfree_heap(tracker);
            kfree_page(pd);
            kprint("[run] Error: dynamic load failed\n");
            return;
        }

        {
            struct task_struct* _newtask = create_task_dynamic(entry, pd, tracker, ctx);
            if (_newtask) {
                _copy_cwd_to_task(_newtask);
                kprint("[run] task created (dynamic), cwd=");
                kprint(_newtask->cwd); kprint("\n");
            } else {
                dynlink_unload_all(ctx);
                kfree_heap(ctx);
                proc_free_pages(tracker);
                kfree_heap(tracker);
                kprint("[run] Error: scheduler refused task\n");
            }
        }

    } else {
        if (force_static)
            kprint("\n[run] --static: skipping dynamic linker\n");

        char _full_path[256];
        if (path[0] == '/') {
            { int _i=0; while(path[_i]&&_i<255){_full_path[_i]=path[_i];_i++;} _full_path[_i]='\0'; }
        } else {
            int ci = 0;
            while (current_path[ci] && ci < 250) { _full_path[ci] = current_path[ci]; ci++; }
            if (ci > 0 && _full_path[ci-1] != '/') _full_path[ci++] = '/';
            for (int i = 0; path[i] && ci < 255; i++) _full_path[ci++] = path[i];
            _full_path[ci] = '\0';
        }
        entry = load_elf(_full_path, pd, tracker);

        if (!entry) {
            proc_free_pages(tracker);
            kfree_heap(tracker);
            kprint("[run] Error: static load failed\n");
            return;
        }

        {
            struct task_struct* _newtask = create_task_with_entry(entry, pd, tracker);
            if (_newtask) {
                _copy_cwd_to_task(_newtask);
                kprint("[run] task created (static), cwd=");
                kprint(_newtask->cwd); kprint("\n");
            } else {
                proc_free_pages(tracker);
                kfree_heap(tracker);
                kprint("[run] Error: scheduler refused task\n");
            }
        }
    }
}

// Отладка
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


// Сеть 
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
    kprint("[commands] registering built-in shell commands\n");
    procfs_register_cmd("cd", 0, cmd_cd);
    procfs_register_cmd("pwd", 0, cmd_pwd);
    procfs_register_cmd("ls", 0, cmd_ls);
    procfs_register_cmd("lsraw", 0, cmd_lsraw);
    procfs_register_cmd("mkdir", 0, cmd_mkdir);
    procfs_register_cmd("rmdir", 0, cmd_rmdir);
    procfs_register_cmd("tch", 0, cmd_tch);
    procfs_register_cmd("rm", 0, cmd_rm);
    procfs_register_cmd("cat", 0, cmd_cat);
    procfs_register_cmd("wrt", 0, cmd_wrt);
    procfs_register_cmd("echo", 0, cmd_echo);
    procfs_register_cmd("mount", 0, cmd_mount);
    procfs_register_cmd("umount", 0, cmd_umount);
    procfs_register_cmd("help", 0, cmd_help);
    procfs_register_cmd("fetch", 0, cmd_fetch);
    procfs_register_cmd("clear", 0, cmd_clear);
    procfs_register_cmd("reboot", 0, cmd_reboot);
    procfs_register_cmd("free", 0, cmd_free);
    procfs_register_cmd("oom", 0, cmd_oom);
    procfs_register_cmd("date", 0, cmd_date);
    procfs_register_cmd("uptime", 0, cmd_uptime);
    procfs_register_cmd("ps", 0, cmd_ps);
    procfs_register_cmd("kill", 0, cmd_kill);
    procfs_register_cmd("run", 0, cmd_run);
    procfs_register_cmd("kbd", 0, cmd_kbd);
    procfs_register_cmd("pic", 0, cmd_pic);
    procfs_register_cmd("ipconfig", 0, cmd_ipconfig);
    procfs_register_cmd("ping", 0, cmd_ping);
    procfs_register_cmd("udptest", 0, cmd_udptest);
    klog(LOG_OK, "commands ready — 29 built-in commands registered");
}