#include "shell.h"
#include "pipe.h"
#include "mntfs.h"
#include "procfs.h"
#include "vfs.h"
#include "kernel.h"
#include "memory.h"
#include "klib.h"

#define DIR_STACK_MAX   32
#define PIPE_PARTS_MAX  8
#define PIPE_BUF_READ   4096

/* ---------- I/O redirection constants ---------- */
#define REDIR_OUT       1   /* >        */
#define REDIR_APPEND    2   /* >>       */
#define REDIR_IN        3   /* <        */
#define REDIR_ERR       4   /* 2>       */
#define REDIR_ERR2OUT   5   /* 2>&1     */

static vfs_node_t* dir_stack[DIR_STACK_MAX];
static int              dir_stack_top = 0;

vfs_node_t* current_dir       = 0;
char             current_path[512] = "/";
vfs_node_t* current_ext4_dir  = 0;
char             current_mnt_path[128] = "";
vfs_node_t* shell_out         = 0;
vfs_node_t* shell_stdin       = 0;
vfs_node_t* shell_stderr      = 0;

static vfs_node_t* cached_cmd_dir = 0;


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
    while (*deeper && node) {
        char c = *deeper++;
        if (c == '/') {
            if (idx > 0) { seg[idx] = '\0'; if (node && node->ops && node->ops->walk) node = node->ops->walk(node, seg); idx = 0; }
        } else { if (idx < 127) seg[idx++] = c; }
    }
    if (idx > 0 && node) { seg[idx]='\0'; if (node->ops && node->ops->walk) node = node->ops->walk(node, seg); }
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

void shell_init(void) {
    kprint("[shell] resetting working directory to VFS root\n");
    shell_resetdir();
    kprint("[shell] clearing cached command directory\n");
    cached_cmd_dir = 0;
    klog(LOG_OK, "shell ready");
}


/* ================================================================
 *  I/O redirection wrapper node
 *  – wraps a target VFS file and tracks the current read/write offset
 *    so that multiple shell_write() / shell_read_stdin() calls work
 *    correctly when redirected to a regular file.
 * ================================================================ */

typedef struct {
    vfs_node_t *target;     /* real file node       */
    uint32_t    pos;        /* current byte offset  */
} redir_wrap_t;

static int _redir_read(vfs_node_t *node, uint32_t off, uint32_t sz, char *buf) {
    redir_wrap_t *rw = (redir_wrap_t *)node->priv;
    (void)off;
    int r = read_vfs(rw->target, rw->pos, sz, buf);
    if (r > 0) rw->pos += (uint32_t)r;
    return r;
}

static int _redir_write(vfs_node_t *node, uint32_t off, uint32_t sz, char *buf) {
    redir_wrap_t *rw = (redir_wrap_t *)node->priv;
    (void)off;
    int r = write_vfs(rw->target, rw->pos, sz, buf);
    if (r > 0) rw->pos += (uint32_t)r;
    return r;
}

static vfs_ops_t _redir_ops = {
    .read  = _redir_read,
    .write = _redir_write,
};

/* Open (or create) a file and return a redirect wrapper node.
   For '>'  pass append=0 (write from offset 0).
   For '>>' pass append=1 (write from current file size). */
static vfs_node_t *_redir_open(const char *path, int append)
{
    vfs_node_t *base = (path[0] == '/')
                       ? vfs_root
                       : (current_dir ? current_dir : vfs_root);
    vfs_node_t *target = vfs_walk_path(base, path);

    if (!target) {
        /* file doesn't exist — try to create it in the current dir */
        vfs_node_t *dir = current_dir ? current_dir : vfs_root;
        if (create_vfs(dir, (char *)path) != 0) return 0;
        target = vfs_walk_path(dir, path);
        if (!target) return 0;
    }

    redir_wrap_t *rw = (redir_wrap_t *)kmalloc(sizeof(redir_wrap_t));
    if (!rw) return 0;
    rw->target = target;
    rw->pos    = append ? target->size : 0;

    vfs_node_t *n = (vfs_node_t *)kmalloc(sizeof(vfs_node_t));
    if (!n) { kfree_heap(rw); return 0; }
    memset(n, 0, sizeof(vfs_node_t));
    copy_string(n->name, "redir");
    n->type = VFS_FILE;
    n->ops  = &_redir_ops;
    n->priv = rw;
    return n;
}

static void _redir_free(vfs_node_t *node)
{
    if (!node) return;
    if (node->ops == &_redir_ops && node->priv)
        kfree_heap(node->priv);     /* free redir_wrap_t */
    kfree_heap(node);               /* free wrapper node */
}

/* ================================================================
 *  Redirect info — parsed from a command segment
 * ================================================================ */

typedef struct {
    int  redir_out;         /* REDIR_OUT | REDIR_APPEND | 0 */
    char out_path[128];
    int  redir_in;          /* REDIR_IN | 0                 */
    char in_path[128];
    int  redir_err;         /* REDIR_ERR | REDIR_ERR2OUT | 0*/
    char err_path[128];
} redir_info_t;

static int _grab_path(char **pp, char *out, int max)
{
    char *p = *pp;
    while (*p == ' ') p++;          

    int i = 0;
    while (*p && *p != ' ' && *p != '>' && *p != '<' && *p != '|') {
        if (i < max - 1)         
            out[i++] = *p;
        p++;
    }
    out[i] = '\0';
    *pp = p;                       
}

static void _parse_redirects(char *cmd, redir_info_t *ri)
{
    memset(ri, 0, sizeof(redir_info_t));

    char *p       = cmd;
    char *cmd_end = 0;   

    while (*p) {
        /* --- 2>&1 --- */
        if (p[0] == '2' && p[1] == '>' && p[2] == '&' && p[3] == '1') {
            if (!cmd_end) cmd_end = p;
            ri->redir_err = REDIR_ERR2OUT;
            p += 4;
            continue;
        }

        /* --- 2>file --- */
        if (p[0] == '2' && p[1] == '>') {
            if (!cmd_end) cmd_end = p;
            ri->redir_err = REDIR_ERR;
            p += 2;
            _grab_path(&p, ri->err_path, 128);
            continue;
        }

        /* --- >>file --- */
        if (p[0] == '>' && p[1] == '>') {
            if (!cmd_end) cmd_end = p;
            ri->redir_out = REDIR_APPEND;
            p += 2;
            _grab_path(&p, ri->out_path, 128);
            continue;
        }

        /* --- >file --- */
        if (p[0] == '>') {
            if (!cmd_end) cmd_end = p;
            ri->redir_out = REDIR_OUT;
            p += 1;
            _grab_path(&p, ri->out_path, 128);
            continue;
        }

        /* --- <file --- */
        if (p[0] == '<') {
            if (!cmd_end) cmd_end = p;
            ri->redir_in = REDIR_IN;
            p += 1;
            _grab_path(&p, ri->in_path, 128);
            continue;
        }

        p++;
    }

    if (cmd_end) {
        char *trim = cmd_end;
        while (trim > cmd && trim[-1] == ' ') trim--;
        *trim = '\0';
    } else {
        /* No operators — just trim trailing spaces. */
        int len = (int)strlen(cmd);
        while (len > 0 && cmd[len - 1] == ' ') cmd[--len] = '\0';
    }
}


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

#define ARGV_MAX 32

typedef struct {
    char        *path;           /* binary name — argv[0]                  */
    char        *argv[ARGV_MAX]; /* NULL-terminated, pointers into segment  */
    int          argc;
    redir_info_t redir;
} command_t;

static int _tokenize_cmd(char *cmd, char **argv, int max)
{
    int   argc = 0;
    char *p    = cmd;

    while (*p && argc < max - 1) {
        while (*p == ' ') p++;      /* skip inter-token spaces */
        if (!*p) break;

        argv[argc++] = p;           /* record token start */

        while (*p && *p != ' ') p++;
        if (*p == ' ') *p++ = '\0'; /* null-terminate token, advance past space */
    }
    argv[argc] = 0;
    return argc;
}


static command_t *parse_line(char *input, int *out_n)
{
    *out_n = 0;
    if (!input || !input[0]) return 0;

    /* 1. Split by '|' into pipe stages ('\0' inserted at each '|'). */
    char *segs[PIPE_PARTS_MAX];
    int   n = split_by(input, '|', segs, PIPE_PARTS_MAX);
    if (n == 0) return 0;

    /* 2. Allocate command array. */
    command_t *cmds = (command_t *)kmalloc((uint32_t)(n * (int)sizeof(command_t)));
    if (!cmds) return 0;
    memset(cmds, 0, (uint32_t)(n * (int)sizeof(command_t)));

    for (int i = 0; i < n; i++) {
        char *seg = segs[i];

        /* Skip leading spaces left by split_by. */
        while (*seg == ' ') seg++;

        /* 3. Parse redirects — null-terminates seg at first operator. */
        _parse_redirects(seg, &cmds[i].redir);

        /* 4. Tokenize remaining command + args into argv. */
        cmds[i].argc = _tokenize_cmd(seg, cmds[i].argv, ARGV_MAX);
        cmds[i].path = (cmds[i].argc > 0) ? cmds[i].argv[0] : 0;
    }

    *out_n = n;
    return cmds;
}

void shell_err_write(const char *s)
{
    if (!s) return;
    if (shell_stderr)
        write_vfs(shell_stderr, 0, strlen(s), (char *)s);
    else
        kprint((char *)s);
}


static vfs_node_t* _get_cmd_dir(void)
{
    if (cached_cmd_dir) return cached_cmd_dir;

    vfs_node_t* pr = procfs_get_root();
    if (pr && pr->ops && pr->ops->walk) {
        cached_cmd_dir = pr->ops->walk(pr, "cmd");
        if (cached_cmd_dir) return cached_cmd_dir;
    }

    vfs_node_t* n = vfs_walk_path(vfs_root, "nvme0n1/sys/proc/cmd");
    if (n) { cached_cmd_dir = n; return n; }

    return 0;
}


static void _run_one(command_t *cmd)
{
    if (!cmd || cmd->argc == 0 || !cmd->path) return;
    if (!current_dir) shell_resetdir();

    vfs_node_t *cmd_dir = _get_cmd_dir();
    if (cmd_dir) {
        vfs_node_t *node = (cmd_dir->ops && cmd_dir->ops->walk)
                           ? cmd_dir->ops->walk(cmd_dir, cmd->path) : 0;
        if (node) {
            char full[512];
            int  off = 0;
            for (int i = 0; i < cmd->argc && off < (int)sizeof(full) - 2; i++) {
                if (i > 0) full[off++] = ' ';
                int n = (int)strlen(cmd->argv[i]);
                if (off + n >= (int)sizeof(full) - 1)
                    n = (int)sizeof(full) - 1 - off;
                memcpy(full + off, cmd->argv[i], (uint32_t)n);
                off += n;
            }
            full[off] = '\0';
            write_vfs(node, 0, (uint32_t)off, full);
            return;
        }
    }

    kprint("\nUnknown command: ");
    kprint(cmd->path);
    kprint("\nType 'help' for a list of commands.\n");
}

static void _run_pipeline(char *input)
{
    int        n;
    command_t *cmds = parse_line(input, &n);
    if (!cmds || n == 0) { kfree_heap(cmds); return; }

    vfs_node_t *prev_read = 0;

    for (int step = 0; step < n; step++) {
        command_t  *cmd     = &cmds[step];
        int         is_last = (step == n - 1);

        vfs_node_t *redir_out_node = 0;
        vfs_node_t *redir_in_node  = 0;
        vfs_node_t *redir_err_node = 0;

        /* ---- set up inter-stage pipe ---- */
        vfs_node_t *pipe_nodes[2] = {0, 0};

        if (!is_last && !cmd->redir.redir_out) {
            /* normal pipe to next stage */
            if (pipe_create(pipe_nodes, 0) != 0) {
                kprint("[pipe] error: pipe_create failed\n");
                shell_out = 0; shell_stdin = 0; shell_stderr = 0;
                if (prev_read) close_vfs(prev_read);
                kfree_heap(cmds);
                return;
            }
            shell_out = pipe_nodes[1];
        } else if (cmd->redir.redir_out) {
            /* stdout redirected to file */
            int append = (cmd->redir.redir_out == REDIR_APPEND);
            redir_out_node = _redir_open(cmd->redir.out_path, append);
            if (!redir_out_node) {
                kprint("[redir] error: cannot open '");
                kprint(cmd->redir.out_path); kprint("'\n");
                shell_out = 0; shell_stdin = 0; shell_stderr = 0;
                if (prev_read) close_vfs(prev_read);
                kfree_heap(cmds);
                return;
            }
            shell_out = redir_out_node;
            /* pipe_nodes stays {0,0} — next stage gets no piped stdin */
        } else {
            /* last stage, no redirect — output to screen */
            shell_out = 0;
        }

        /* ---- stdin: from previous pipe or from file ---- */
        if (cmd->redir.redir_in) {
            redir_in_node = _redir_open(cmd->redir.in_path, 0);
            if (!redir_in_node) {
                kprint("[redir] error: cannot open '");
                kprint(cmd->redir.in_path); kprint("'\n");
                _redir_free(redir_out_node);
                if (!is_last && pipe_nodes[0]) {
                    close_vfs(pipe_nodes[0]);
                    close_vfs(pipe_nodes[1]);
                }
                shell_out = 0; shell_stdin = 0; shell_stderr = 0;
                if (prev_read) close_vfs(prev_read);
                kfree_heap(cmds);
                return;
            }
            shell_stdin = redir_in_node;
            /* close unused prev_read when stdin is redirected from file */
            if (prev_read) { close_vfs(prev_read); prev_read = 0; }
        } else {
            shell_stdin = prev_read;
        }

        /* ---- stderr ---- */
        if (cmd->redir.redir_err == REDIR_ERR2OUT) {
            shell_stderr = shell_out;   /* merge stderr into stdout */
        } else if (cmd->redir.redir_err == REDIR_ERR) {
            redir_err_node = _redir_open(cmd->redir.err_path, 0);
            if (redir_err_node)
                shell_stderr = redir_err_node;
        } else {
            shell_stderr = 0;
        }

        /* ---- execute ---- */
        _run_one(cmd);

        /* ---- per-stage cleanup ---- */
        if (!is_last && pipe_nodes[1])
            close_vfs(pipe_nodes[1]);
        shell_out = 0;

        if (prev_read && !cmd->redir.redir_in) {
            close_vfs(prev_read);
            prev_read = 0;
        }

        _redir_free(redir_out_node);
        _redir_free(redir_in_node);
        _redir_free(redir_err_node);
        shell_stderr = 0;

        if (!is_last)
            prev_read = pipe_nodes[0];
    }

    if (prev_read) close_vfs(prev_read);
    shell_out    = 0;
    shell_stdin  = 0;
    shell_stderr = 0;

    kfree_heap(cmds);
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

        _run_pipeline(seg);
    }
}