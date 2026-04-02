#include "shm.h"
#include "memory.h"
#include "kernel.h"
#include "task.h"
#include "sync.h"
#include "libc.h"

/* Virtual address range reserved for SHM attachments (below mmap base). */
#define SHM_VA_BASE   0x30000000u
#define SHM_VA_LIMIT  0x40000000u

/* Maximum physical pages per segment (256 * 4096 = 1 MB). */
#define SHM_MAX_PAGES 256

/* Maximum number of segments in the system. */
#define SHM_MAX_SEGMENTS 64

typedef struct {
    int      key;
    int      perms;
    uint32_t size;                    /* requested size in bytes  */
    uint32_t num_pages;
    void*    pages[SHM_MAX_PAGES];    /* physical page pointers   */
    uint32_t nattch;                  /* current attach count     */
    uint32_t cpid;                    /* creator PID              */
    uint32_t lpid;                    /* PID of last shmat/shmdt  */
    int      valid;
    int      destroy;                 /* IPC_RMID called; free when nattch==0 */
} shm_seg_t;

static shm_seg_t      shm_table[SHM_MAX_SEGMENTS];
static irq_spinlock_t shm_lock;
static int            shm_initialized = 0;

/* ------------------------------------------------------------------ */

static void shm_ensure_init(void) {
    if (shm_initialized) return;
    irq_spinlock_init(&shm_lock);
    for (int i = 0; i < SHM_MAX_SEGMENTS; i++) {
        shm_table[i].valid  = 0;
        shm_table[i].nattch = 0;
        shm_table[i].destroy = 0;
    }
    shm_initialized = 1;
}

static int seg_valid(int id) {
    return id >= 1 && id <= SHM_MAX_SEGMENTS && shm_table[id - 1].valid;
}

/* Free physical pages of a segment and mark it invalid.
   Caller must hold shm_lock. */
static void seg_free(shm_seg_t* s) {
    for (uint32_t i = 0; i < s->num_pages; i++) {
        if (s->pages[i]) {
            kfree_page(s->pages[i]);
            s->pages[i] = 0;
        }
    }
    s->valid  = 0;
    s->nattch = 0;
}

/* ------------------------------------------------------------------ */

int shm_get(int key, uint32_t size, int flags) {
    shm_ensure_init();

    irq_spinlock_acquire(&shm_lock);

    /* Search for an existing segment with this key (unless IPC_PRIVATE). */
    if (key != IPC_PRIVATE) {
        for (int i = 0; i < SHM_MAX_SEGMENTS; i++) {
            if (!shm_table[i].valid) continue;
            if (shm_table[i].key != key) continue;

            if ((flags & IPC_CREAT) && (flags & IPC_EXCL)) {
                irq_spinlock_release(&shm_lock);
                return -1;   /* EEXIST */
            }
            int id = i + 1;
            irq_spinlock_release(&shm_lock);
            return id;
        }
        if (!(flags & IPC_CREAT)) {
            irq_spinlock_release(&shm_lock);
            return -1;   /* ENOENT */
        }
    }

    if (size == 0) {
        irq_spinlock_release(&shm_lock);
        return -1;
    }

    /* Find a free slot. */
    int slot = -1;
    for (int i = 0; i < SHM_MAX_SEGMENTS; i++) {
        if (!shm_table[i].valid) { slot = i; break; }
    }
    if (slot < 0) {
        irq_spinlock_release(&shm_lock);
        return -1;   /* ENOSPC */
    }

    uint32_t num_pages = (size + PAGE_SIZE - 1) / PAGE_SIZE;
    if (num_pages > SHM_MAX_PAGES) {
        irq_spinlock_release(&shm_lock);
        return -1;   /* EINVAL – too large */
    }

    /* Allocate and zero physical pages. */
    for (uint32_t i = 0; i < num_pages; i++) {
        void* p = kalloc();
        if (!p) {
            for (uint32_t j = 0; j < i; j++) {
                kfree_page(shm_table[slot].pages[j]);
                shm_table[slot].pages[j] = 0;
            }
            irq_spinlock_release(&shm_lock);
            return -1;   /* ENOMEM */
        }
        uint8_t* b = (uint8_t*)p;
        for (uint32_t k = 0; k < PAGE_SIZE; k++) b[k] = 0;
        shm_table[slot].pages[i] = p;
    }

    shm_table[slot].key       = key;
    shm_table[slot].perms     = flags & 0777;
    shm_table[slot].size      = size;
    shm_table[slot].num_pages = num_pages;
    shm_table[slot].nattch    = 0;
    shm_table[slot].cpid      = current_task ? current_task->pid : 0;
    shm_table[slot].lpid      = 0;
    shm_table[slot].valid     = 1;
    shm_table[slot].destroy   = 0;

    irq_spinlock_release(&shm_lock);
    return slot + 1;
}

/* ------------------------------------------------------------------ */

/* Find a free VA in the SHM range for `num_pages` pages within current_task.
   Caller must hold shm_lock. */
static uint32_t find_shm_va(uint32_t num_pages) {
    uint32_t size      = num_pages * PAGE_SIZE;
    uint32_t candidate = SHM_VA_BASE;

    while (candidate + size <= SHM_VA_LIMIT) {
        int clash = 0;
        for (int i = 0; i < TASK_SHM_MAX; i++) {
            int id = current_task->shm_attachments[i].shm_id;
            if (!id || !seg_valid(id)) continue;

            shm_seg_t* s    = &shm_table[id - 1];
            uint32_t   base = current_task->shm_attachments[i].shm_vaddr;
            uint32_t   end  = base + s->num_pages * PAGE_SIZE;
            uint32_t   cend = candidate + size;

            if (candidate < end && cend > base) {
                candidate = (end + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1u);
                clash = 1;
                break;
            }
        }
        if (!clash) return candidate;
    }
    return 0;   /* no space */
}

/* Unmap shared pages from pd WITHOUT freeing physical memory. */
static void shm_unmap_from(uint32_t* pd, uint32_t va, uint32_t num_pages) {
    for (uint32_t i = 0; i < num_pages; i++) {
        uint32_t addr = va + i * PAGE_SIZE;
        uint32_t pdi  = (addr >> 22) & 0x3FF;
        if (!(pd[pdi] & PAGE_PRESENT)) continue;
        uint32_t* pt  = (uint32_t*)(pd[pdi] & ~0xFFFu);
        uint32_t  pti = (addr >> 12) & 0x3FF;
        pt[pti] = 0;
        __asm__ __volatile__("invlpg (%0)" :: "r"(addr) : "memory");
    }
}

/* ------------------------------------------------------------------ */

uint32_t shm_at(int shmid, uint32_t shmaddr, int flags) {
    shm_ensure_init();

    if (!current_task || current_task->is_kernel)
        return (uint32_t)-1;

    irq_spinlock_acquire(&shm_lock);

    if (!seg_valid(shmid)) {
        irq_spinlock_release(&shm_lock);
        return (uint32_t)-1;
    }

    /* Find a free attachment slot. */
    int slot = -1;
    for (int i = 0; i < TASK_SHM_MAX; i++) {
        if (!current_task->shm_attachments[i].shm_id) { slot = i; break; }
    }
    if (slot < 0) {
        irq_spinlock_release(&shm_lock);
        return (uint32_t)-1;   /* EMFILE */
    }

    shm_seg_t* seg = &shm_table[shmid - 1];

    uint32_t va;
    if (shmaddr != 0) {
        if (flags & SHM_RND)
            shmaddr &= ~(PAGE_SIZE - 1u);
        if (shmaddr % PAGE_SIZE != 0) {
            irq_spinlock_release(&shm_lock);
            return (uint32_t)-1;   /* EINVAL */
        }
        va = shmaddr;
    } else {
        va = find_shm_va(seg->num_pages);
        if (va == 0) {
            irq_spinlock_release(&shm_lock);
            return (uint32_t)-1;   /* ENOMEM */
        }
    }

    int page_flags = PAGE_PRESENT | PAGE_USER;
    if (!(flags & SHM_RDONLY)) page_flags |= PAGE_RW;

    for (uint32_t i = 0; i < seg->num_pages; i++) {
        vmm_map(current_task->page_directory,
                va + i * PAGE_SIZE,
                (uint32_t)seg->pages[i],
                page_flags);
    }

    current_task->shm_attachments[slot].shm_id    = shmid;
    current_task->shm_attachments[slot].shm_vaddr = va;
    seg->nattch++;
    seg->lpid = current_task->pid;

    irq_spinlock_release(&shm_lock);
    return va;
}

/* ------------------------------------------------------------------ */

int shm_dt(uint32_t shmaddr) {
    shm_ensure_init();

    if (!current_task || current_task->is_kernel)
        return -1;

    irq_spinlock_acquire(&shm_lock);

    /* Find the attachment by virtual address. */
    int slot = -1;
    for (int i = 0; i < TASK_SHM_MAX; i++) {
        if (current_task->shm_attachments[i].shm_vaddr == shmaddr &&
            current_task->shm_attachments[i].shm_id   != 0) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        irq_spinlock_release(&shm_lock);
        return -1;   /* EINVAL – not attached */
    }

    int id = current_task->shm_attachments[slot].shm_id;
    if (!seg_valid(id)) {
        current_task->shm_attachments[slot].shm_id    = 0;
        current_task->shm_attachments[slot].shm_vaddr = 0;
        irq_spinlock_release(&shm_lock);
        return -1;
    }

    shm_seg_t* seg = &shm_table[id - 1];

    shm_unmap_from(current_task->page_directory, shmaddr, seg->num_pages);

    current_task->shm_attachments[slot].shm_id    = 0;
    current_task->shm_attachments[slot].shm_vaddr = 0;

    if (seg->nattch > 0) seg->nattch--;
    seg->lpid = current_task->pid;

    if (seg->destroy && seg->nattch == 0)
        seg_free(seg);

    irq_spinlock_release(&shm_lock);
    return 0;
}

/* ------------------------------------------------------------------ */

int shm_ctl(int shmid, int cmd, void* buf) {
    shm_ensure_init();

    irq_spinlock_acquire(&shm_lock);

    if (!seg_valid(shmid)) {
        irq_spinlock_release(&shm_lock);
        return -1;
    }

    shm_seg_t* seg = &shm_table[shmid - 1];

    if (cmd == IPC_RMID) {
        seg->destroy = 1;
        if (seg->nattch == 0)
            seg_free(seg);
        irq_spinlock_release(&shm_lock);
        return 0;
    }

    if (cmd == IPC_STAT) {
        if (!buf) {
            irq_spinlock_release(&shm_lock);
            return -1;
        }
        shminfo_t* info = (shminfo_t*)buf;
        info->shm_segsz  = seg->size;
        info->shm_cpid   = seg->cpid;
        info->shm_lpid   = seg->lpid;
        info->shm_nattch = seg->nattch;
        irq_spinlock_release(&shm_lock);
        return 0;
    }

    irq_spinlock_release(&shm_lock);
    return -1;   /* EINVAL – unknown cmd */
}

/* ------------------------------------------------------------------ */

/* Detach all shared-memory segments of the task identified by `pid`.
   Pages are unmapped from `page_directory` without being freed.
   Called on exec and on task reap. */
void shm_detach_all(uint32_t pid, uint32_t* page_directory) {
    shm_ensure_init();

    if (!page_directory) return;

    /* Find the task struct by PID. */
    struct task_struct* found = 0;
    struct task_struct* cur   = task_list_head;
    if (!cur) return;

    do {
        if (cur->pid == pid) { found = cur; break; }
        cur = cur->next;
    } while (cur && cur != task_list_head);

    if (!found) return;

    irq_spinlock_acquire(&shm_lock);

    for (int i = 0; i < TASK_SHM_MAX; i++) {
        int id = found->shm_attachments[i].shm_id;
        if (!id) continue;
        if (!seg_valid(id)) {
            found->shm_attachments[i].shm_id    = 0;
            found->shm_attachments[i].shm_vaddr = 0;
            continue;
        }

        shm_seg_t* seg = &shm_table[id - 1];

        shm_unmap_from(page_directory,
                       found->shm_attachments[i].shm_vaddr,
                       seg->num_pages);

        found->shm_attachments[i].shm_id    = 0;
        found->shm_attachments[i].shm_vaddr = 0;

        if (seg->nattch > 0) seg->nattch--;
        seg->lpid = pid;

        if (seg->destroy && seg->nattch == 0)
            seg_free(seg);
    }

    irq_spinlock_release(&shm_lock);
}
