/*
 * task_comm.c — per-process command names (the value behind /proc/<pid>/comm).
 *
 * The name deliberately does NOT live in struct task_struct or
 * proc_metadata_t: both are mirrored in Rust (cact_sync::task_abi,
 * rust_mm::ffi) with compile-time offset/size assertions, so growing either
 * would drag the scheduler's ABI along.  A small pid-keyed side table avoids
 * that entirely — it is filled on fork (inherited) and exec (basename of the
 * new image), which is where the name becomes known.
 */

#include <stdint.h>
#include "task.h"
#include "sync.h"
#include "klib.h"

typedef struct {
    uint32_t pid;                   /* 0 = free slot */
    char     name[TASK_COMM_LEN];
} task_comm_ent_t;

static task_comm_ent_t comm_table[TASK_COMM_SLOTS];
/* Zero-initialised BSS is a valid unlocked irq_spinlock_t (locked == 0). */
static irq_spinlock_t  comm_lock;

/* Caller must hold comm_lock.  Returns the pid's slot, or the first free slot
 * when create != 0 (NULL if the table is full). */
static task_comm_ent_t *_slot(uint32_t pid, int create) {
    task_comm_ent_t *free_slot = 0;
    for (int i = 0; i < TASK_COMM_SLOTS; i++) {
        if (comm_table[i].pid == pid) return &comm_table[i];
        if (!free_slot && comm_table[i].pid == 0) free_slot = &comm_table[i];
    }
    return create ? free_slot : 0;
}

static const char *_base(const char *p) {
    const char *b = p;
    for (const char *s = p; *s; s++)
        if (*s == '/') b = s + 1;
    return b;
}

void task_comm_set(uint32_t pid, const char *name) {
    if (pid == 0 || !name || !name[0]) return;

    irq_spinlock_acquire(&comm_lock);
    task_comm_ent_t *e = _slot(pid, 1);
    if (e) {
        e->pid = pid;
        int i = 0;
        for (; name[i] && i < TASK_COMM_LEN - 1; i++) e->name[i] = name[i];
        e->name[i] = '\0';
    }
    irq_spinlock_release(&comm_lock);
}

void task_comm_set_path(uint32_t pid, const char *path) {
    if (!path || !path[0]) return;
    task_comm_set(pid, _base(path));
}

void task_comm_inherit(uint32_t pid, uint32_t parent_pid) {
    char buf[TASK_COMM_LEN];
    if (task_comm_get(parent_pid, buf, (int)sizeof(buf)) > 0)
        task_comm_set(pid, buf);
}

int task_comm_get(uint32_t pid, char *out, int cap) {
    if (!out || cap <= 0 || pid == 0) return -1;
    out[0] = '\0';

    irq_spinlock_acquire(&comm_lock);
    task_comm_ent_t *e = _slot(pid, 0);
    int n = -1;
    if (e && e->pid == pid) {
        int i = 0;
        for (; e->name[i] && i < cap - 1; i++) out[i] = e->name[i];
        out[i] = '\0';
        n = i;
    }
    irq_spinlock_release(&comm_lock);
    return n;
}
