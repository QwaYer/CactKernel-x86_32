#ifndef CCTKFS_TREE_H
#define CCTKFS_TREE_H

#include <stdint.h>
#include "vfs.h"

/* A synthesized read-only directory tree built from cctkfs archive paths.
 *
 * Every overlay filesystem forwards part of its namespace to the boot archive
 * (see initfs_modblob).  The archive stores flat canonical paths, e.g.
 * "/usr/lib/cact-install/grub/i386-pc/core.img", so an overlay that only
 * matched basenames would hide everything below the first level.  cctkfs_tree
 * rebuilds the real hierarchy: intermediate directories are synthesized and
 * every leaf reads straight out of the staged archive buffer (no copies).
 *
 * Callers keep the handle in .bss and build it once at init time:
 *
 *   static cctkfs_tree_t tree;
 *   cctkfs_tree_build(&tree, "/usr/lib/", 0755, 0);
 *   vfs_node_t *n = cctkfs_tree_walk(&tree, "modules");
 */
typedef struct cctkfs_tree {
    const char *prefix;         /* archive prefix, e.g. "/usr/lib/" */
    int         prefix_len;
    uint32_t    root_file_mode; /* mode for files directly below the prefix */
    const char *const *exclude; /* first-level names to leave out, or 0 */
    void       *root;           /* opaque: synthesized root node */
} cctkfs_tree_t;

/* Cursor over one directory level (the tree root, or a child list). */
typedef struct cctkfs_iter {
    void *cur;
} cctkfs_iter_t;

/* Build the tree from every archive entry below `prefix` (the prefix itself is
 * stripped from the stored names).  Files directly below the prefix get
 * `root_file_mode`; deeper ones get 0644.  `exclude` is an optional
 * NULL-terminated list of first-level names to leave out (used when another
 * filesystem is mounted at that name).  Building an already built tree is a
 * no-op. */
void        cctkfs_tree_build (cctkfs_tree_t *tree, const char *prefix,
                               uint32_t root_file_mode,
                               const char *const *exclude);

/* The synthesized root directory node; the caller names it (its name is empty
 * until then).  0 when the tree was not built. */
vfs_node_t *cctkfs_tree_root  (cctkfs_tree_t *tree);

/* Look up a first-level name; 0 when missing. */
vfs_node_t *cctkfs_tree_walk  (cctkfs_tree_t *tree, const char *name);

/* First-level iteration.  cctkfs_tree_first() also seeds the cursor. */
vfs_node_t *cctkfs_tree_first (cctkfs_tree_t *tree, cctkfs_iter_t *it);
vfs_node_t *cctkfs_iter_next  (cctkfs_iter_t *it);

/* Recursive node counters (dirs counts every synthesized subdirectory). */
void        cctkfs_tree_count (cctkfs_tree_t *tree, uint32_t *files,
                               uint32_t *dirs);

#endif /* CCTKFS_TREE_H */
