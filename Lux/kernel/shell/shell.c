#include "shell.h"
#include "pipe.h"
#include "mntfs.h"
#include "vfs.h"
#include "kernel.h"
#include "memory.h"
#include "libc.h"

#define DIR_STACK_MAX   32
#define PIPE_PARTS_MAX  8
#define PIPE_BUF_READ   4096

static vfs_node_t* dir_stack[DIR_STACK_MAX];
static int              dir_stack_top = 0;

vfs_node_t* current_dir       = 0;
char             current_path[512] = "/";
vfs_node_t* current_ext4_dir  = 0;
char             current_mnt_path[128] = "";
vfs_node_t* shell_out         = 0;
vfs_node_t* shell_stdin       = 0;


void shell_write(const char* s)
{
    if (!s) return;
    if (shell_out)
        write_vfs(shell_out, 0, strlen(s), (char*)s);
    else
        kprint((char*)s);
}

char* shell_read_stdin(int* out_len)
{
    if (!shell_stdin) { if (out_len) *out_len = 0; return 0; }

    char* buf = (char*)kmalloc(PIPE_BUF_READ + 1);
    if (!buf)  { if (out_len) *out_len = 0; return 0; }

    int total = 0;
    int n;
    while (total < PIPE_BUF_READ) {
        n = read_vfs(shell_stdin, 0, PIPE_BUF_READ - total, buf + total);
        if (n <= 0) break;
        total += n;
    }
    buf[total] = '\0';
    if (out_len) *out_len = total;
    return buf;
}


void shell_pushdir(vfs_node_t* node) {
    if (dir_stack_top < DIR_STACK_MAX - 1)
        dir_stack[dir_stack_top++] = node;
}

void shell_recalc_ext4_dir(void) {
    current_ext4_dir  = 0;
    current_mnt_path[0] = '\0';

    const char *cp = current_path;
    while (*cp == '/') cp++;
    if (!*cp) return;

    char devname[32]; int di = 0;
    while (cp[di] && cp[di] != '/' && di < 31) { devname[di] = cp[di]; di++; }
    devname[di] = '\0';

    const char *rest = (cp[di] == '/') ? cp + di + 1 : "";
    char second[32]; int si = 0;
    while (rest[si] && rest[si] != '/' && si < 31) { second[si] = rest[si]; si++; }
    second[si] = '\0';

    if (compare_string(second, "raw") != 0) return;

    vfs_node_t *ext4r = mntfs_get_ext4(devname);
    if (!ext4r) return;

    strncpy(current_mnt_path, devname, 128);

    const char *deeper = (rest[si] == '/') ? rest + si + 1 : "";
    if (!deeper[0]) { current_ext4_dir = ext4r; return; }

    vfs_node_t *node = ext4r;
    char seg[128]; int idx = 0;
    while (*deeper) {
        char c = *deeper++;
        if (c == '/') {
            if (idx > 0) { seg[idx] = '\0'; if (node->ops && node->ops->walk) node = node->ops->walk(node, seg); idx = 0; }
        } else { seg[idx++] = c; }
    }
    if (idx > 0) { seg[idx]='\0'; if (node->ops && node->ops->walk) node = node->ops->walk(node, seg); }
    current_ext4_dir = node;
}

void shell_popdir(void) {
    if (dir_stack_top > 0) {
        dir_stack_top--;
        current_dir = dir_stack[dir_stack_top]
                      ? dir_stack[dir_stack_top] : vfs_root;
        int len = strlen(current_path);
        while (len > 1 && current_path[len-1] != '/') len--;
        if (len > 1) len--;
        current_path[len] = '\0';
        if (len == 0) copy_string(current_path, "/");
    } else {
        current_dir = vfs_root;
        copy_string(current_path, "/");
    }
    shell_recalc_ext4_dir();
}

void shell_resetdir(void) {
    dir_stack_top        = 0;
    dir_stack[0]         = 0;
    current_dir          = vfs_root;
    current_ext4_dir     = 0;
    current_mnt_path[0]  = '\0';
    copy_string(current_path, "/");
}

void shell_init(void) { shell_resetdir(); }


static int split_by(char* input, char sep, char* parts[], int max)
{
    int count = 0;
    char* p   = input;

    while (count < max) {
        while (*p == ' ') p++;
        if (*p == '\0') break;

        parts[count++] = p;

        while (*p && *p != sep) p++;
        if (*p == '\0') break;

        char* end = p - 1;
        while (end > parts[count-1] && *end == ' ') { *end = '\0'; end--; }

        *p++ = '\0';
    }
    return count;
}


static void _run_one(char* input)
{
    if (!input || input[0] == '\0') return;
    if (!current_dir) shell_resetdir();

    char cmd_name[64];
    int i = 0;
    while (input[i] && input[i] != ' ' && i < 63) {
        cmd_name[i] = input[i];
        i++;
    }
    cmd_name[i] = '\0';

    vfs_node_t* cmd_node = vfs_walk_path(0, "hda/sys/proc/cmd");
    if (cmd_node) {
        vfs_node_t* node = cmd_node->ops && cmd_node->ops->walk
                           ? cmd_node->ops->walk(cmd_node, cmd_name) : 0;
        if (node) {
            write_vfs(node, 0, (uint32_t)strlen(input), (char*)input);
            return;
        }
    }

    kprint("\nUnknown command: ");
    kprint(cmd_name);
    kprint("\nType 'help' for a list of commands.\n");
}


static void _run_pipeline(char* parts[], int n)
{
    vfs_node_t* prev_read = 0;

    for (int step = 0; step < n; step++) {
        int is_last = (step == n - 1);

        vfs_node_t* pipe_nodes[2] = {0, 0};

        if (!is_last) {
            if (pipe_create(pipe_nodes, 0) != 0) {
                kprint("[pipe] error: pipe_create failed\n");
                shell_out   = 0;
                shell_stdin = 0;
                if (prev_read) close_vfs(prev_read);
                return;
            }
            shell_out = pipe_nodes[1];  
        } else {
            shell_out = 0;              
        }

        shell_stdin = prev_read;        

        _run_one(parts[step]);

        if (!is_last) {
            close_vfs(pipe_nodes[1]);
            shell_out = 0;
        }

        if (prev_read) {
            close_vfs(prev_read);
            prev_read = 0;
        }

        if (!is_last)
            prev_read = pipe_nodes[0];
    }

    if (prev_read) close_vfs(prev_read);
    shell_out   = 0;
    shell_stdin = 0;
}


void shell_execute(char* input)
{
    if (!input) return;

    while (*input && (*input < 0x20 || *input == ' ')) input++;
    if (*input == '\0') return;

    int len = strlen(input);
    while (len > 0 && (input[len-1] < 0x20 || input[len-1] == ' '))
        input[--len] = '\0';
    if (len == 0) return;

    char* seq_parts[PIPE_PARTS_MAX];
    int   seq_n = split_by(input, ';', seq_parts, PIPE_PARTS_MAX);

    for (int s = 0; s < seq_n; s++) {
        char* seg = seq_parts[s];
        if (!seg || seg[0] == '\0') continue;

        char* pipe_parts[PIPE_PARTS_MAX];
        int   pipe_n = split_by(seg, '|', pipe_parts, PIPE_PARTS_MAX);

        if (pipe_n < 2) {
            shell_out   = 0;
            shell_stdin = 0;
            _run_one(seg);
        } else {
            _run_pipeline(pipe_parts, pipe_n);
        }
    }
}