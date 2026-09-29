#include "cctkfs_tree.h"
#include "initfs_modblob.h"
#include "kernel.h"
#include "memory.h"
#include "klib.h"

/* One node of the synthesized tree.  A directory owns `child`; a file owns
 * `data`/`size`.  `de` is that directory's readdir scratch. */
typedef struct tree_node {
    vfs_node_t         node;
    struct tree_node  *next;    /* next sibling */
    struct tree_node  *child;   /* first child (directories only) */
    const uint8_t     *data;    /* file payload */
    vfs_dirent_t       de;      /* readdir scratch (directories) */
} tree_node_t;

static int has_prefix(const char *s, const char *pre) {
    while (*pre) {
        if (*s++ != *pre++) return 0;
    }
    return 1;
}

static int name_eq_n(const char *a, const char *b, int len) {
    for (int i = 0; i < len; i++)
        if (!a[i] || a[i] != b[i]) return 0;
    return 1;
}

/* ── leaf files ─────────────────────────────────────────────────────────── */

static int _file_read(vfs_node_t *node, uint32_t off, uint32_t size,
                      char *buf) {
    tree_node_t *n = (tree_node_t *)node->priv;
    if (!n || !buf) return 0;
    if (off >= n->node.size) return 0;
    uint32_t avail = n->node.size - off;
    uint32_t cnt   = size < avail ? size : avail;
    memcpy(buf, (const char *)n->data + off, cnt);
    return (int)cnt;
}

static vfs_ops_t file_ops = {
    .read = _file_read,
};

/* ── directories ────────────────────────────────────────────────────────── */

static vfs_node_t *_list_walk(tree_node_t *list, const char *name) {
    for (tree_node_t *c = list; c; c = c->next)
        if (streq(c->node.name, name)) return &c->node;
    return 0;
}

static vfs_dirent_t *_list_readdir(tree_node_t *list, uint32_t index,
                                   vfs_dirent_t *scratch) {
    for (tree_node_t *c = list; c; c = c->next) {
        if (index-- == 0) {
            strlcpy(scratch->name, c->node.name, 128);
            scratch->inode = 1;
            return scratch;
        }
    }
    return 0;
}

static vfs_node_t *_dir_walk(vfs_node_t *dir, const char *name) {
    tree_node_t *d = (tree_node_t *)dir->priv;
    if (!d) return 0;
    return _list_walk(d->child, name);
}

static vfs_dirent_t *_dir_readdir(vfs_node_t *dir, uint32_t index) {
    tree_node_t *d = (tree_node_t *)dir->priv;
    if (!d) return 0;
    return _list_readdir(d->child, index, &d->de);
}

static void _dir_listdir(vfs_node_t *dir) {
    tree_node_t *d = (tree_node_t *)dir->priv;
    if (!d || !d->child) { printk("  (empty)\n"); return; }
    for (tree_node_t *c = d->child; c; c = c->next) {
        printk("  "); printk(c->node.name);
        printk(c->node.type == VFS_DIRECTORY ? "/" : "  [cctkfs]");
        printk("\n");
    }
}

static vfs_ops_t dir_ops = {
    .walk    = _dir_walk,
    .readdir = _dir_readdir,
    .listdir = _dir_listdir,
};

/* ── construction ───────────────────────────────────────────────────────── */

static void _append(tree_node_t **headp, tree_node_t *n) {
    n->next = 0;
    if (!*headp) { *headp = n; return; }
    tree_node_t *t = *headp;
    while (t->next) t = t->next;
    t->next = n;
}

static tree_node_t *_find(tree_node_t *list, const char *name, int len) {
    for (tree_node_t *c = list; c; c = c->next)
        if (name_eq_n(c->node.name, name, len) && c->node.name[len] == '\0')
            return c;
    return 0;
}

/* The kernel heap does not zero, so every node is cleared here. */
static tree_node_t *_new_node(const char *name, int len, uint32_t type,
                              uint32_t mode, vfs_ops_t *ops) {
    tree_node_t *n = (tree_node_t *)kmalloc(sizeof(tree_node_t));
    if (!n) return 0;
    memset(n, 0, sizeof(tree_node_t));
    int copy = len < 127 ? len : 127;
    for (int i = 0; i < copy; i++) n->node.name[i] = name[i];
    n->node.name[copy] = '\0';
    n->node.type = type;
    n->node.mode = mode;
    n->node.ops  = ops;
    n->node.priv = n;
    return n;
}

static tree_node_t *_mkdir(tree_node_t **headp, const char *name, int len) {
    tree_node_t *d = _find(*headp, name, len);
    if (d) return d;
    d = _new_node(name, len, VFS_DIRECTORY, 0755, &dir_ops);
    if (!d) return 0;
    _append(headp, d);
    return d;
}

static int _excluded(const char *const *exclude, const char *name, int len) {
    if (!exclude) return 0;
    for (int i = 0; exclude[i]; i++) {
        const char *e = exclude[i];
        int j = 0;
        while (j < len && e[j] && e[j] == name[j]) j++;
        if (j == len && !e[j]) return 1;
    }
    return 0;
}

/* Insert "<prefix-relative path>", creating the directories in between. */
static void _insert(cctkfs_tree_t *tree, const char *rel, const uint8_t *data,
                    uint32_t size) {
    tree_node_t  *root  = (tree_node_t *)tree->root;
    tree_node_t **headp = &root->child;
    const char   *p     = rel;

    for (;;) {
        const char *slash = p;
        while (*slash && *slash != '/') slash++;
        int len = (int)(slash - p);
        if (len <= 0) return;

        if (!*slash) {                       /* final component: the file */
            if (_find(*headp, p, len)) return;             /* first wins */
            tree_node_t *f = _new_node(p, len, VFS_FILE,
                                       (headp == &root->child)
                                           ? tree->root_file_mode : 0644,
                                       &file_ops);
            if (!f) return;
            f->node.size = size;
            f->data      = data;
            _append(headp, f);
            return;
        }

        if (headp == &root->child && _excluded(tree->exclude, p, len)) return;
        tree_node_t *d = _mkdir(headp, p, len);
        if (!d) return;
        headp = &d->child;
        p     = slash + 1;
    }
}

void cctkfs_tree_build(cctkfs_tree_t *tree, const char *prefix,
                       uint32_t root_file_mode, const char *const *exclude) {
    if (!tree || tree->root) return;                   /* build once */

    tree->prefix         = prefix;
    tree->prefix_len     = (int)strlen(prefix);
    tree->root_file_mode = root_file_mode;
    tree->exclude        = exclude;

    tree_node_t *root = _new_node("", 0, VFS_DIRECTORY, 0755, &dir_ops);
    if (!root) return;
    tree->root = root;

    int n = initfs_modblob_count();
    for (int i = 0; i < n; i++) {
        const char *path;
        const uint8_t *data;
        uint32_t sz;
        if (initfs_modblob_at(i, &path, &data, &sz) != 0) continue;
        if (!has_prefix(path, prefix)) continue;
        _insert(tree, path + tree->prefix_len, data, sz);
    }
}

/* ── accessors ──────────────────────────────────────────────────────────── */

static tree_node_t *_root_of(cctkfs_tree_t *tree) {
    return (tree && tree->root) ? (tree_node_t *)tree->root : 0;
}

vfs_node_t *cctkfs_tree_root(cctkfs_tree_t *tree) {
    tree_node_t *root = _root_of(tree);
    return root ? &root->node : 0;
}

vfs_node_t *cctkfs_tree_walk(cctkfs_tree_t *tree, const char *name) {
    tree_node_t *root = _root_of(tree);
    if (!root) return 0;
    return _list_walk(root->child, name);
}

vfs_node_t *cctkfs_tree_first(cctkfs_tree_t *tree, cctkfs_iter_t *it) {
    tree_node_t *root = _root_of(tree);
    it->cur = root ? root->child : 0;
    return it->cur ? &((tree_node_t *)it->cur)->node : 0;
}

vfs_node_t *cctkfs_iter_next(cctkfs_iter_t *it) {
    tree_node_t *n = (tree_node_t *)it->cur;
    if (!n) return 0;
    it->cur = n->next;
    return n->next ? &n->next->node : 0;
}

static void _count(tree_node_t *list, uint32_t *files, uint32_t *dirs) {
    for (tree_node_t *c = list; c; c = c->next) {
        if (c->node.type == VFS_DIRECTORY) {
            (*dirs)++;
            _count(c->child, files, dirs);
        } else {
            (*files)++;
        }
    }
}

void cctkfs_tree_count(cctkfs_tree_t *tree, uint32_t *files, uint32_t *dirs) {
    tree_node_t *root = _root_of(tree);
    if (root) _count(root->child, files, dirs);
}
