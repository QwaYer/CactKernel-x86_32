#include "shm.h"
#include "memory.h"
#include "task.h"
#include "kernel.h"
#include "libc.h"
#include "mmap.h"

static shm_segment_t shm_table[SHM_MAX_SEGMENTS];
static int shm_next_id = 1;

void shm_init(void)
{
    for (int i = 0; i < SHM_MAX_SEGMENTS; i++) {
        shm_table[i].used = 0;
        shm_table[i].id   = 0;
    }
    kprint("[SHM] shm_init: table ready (");
    char buf[16]; itoa(SHM_MAX_SEGMENTS, buf); kprint(buf);
    kprint(" slots)\n");
}

static shm_segment_t* find_by_id(int shmid)
{
    for (int i = 0; i < SHM_MAX_SEGMENTS; i++) {
        if (shm_table[i].used && shm_table[i].id == shmid)
            return &shm_table[i];
    }
    return 0;
}

static shm_segment_t* find_by_key(int key)
{
    if (key == IPC_PRIVATE) return 0;
    for (int i = 0; i < SHM_MAX_SEGMENTS; i++) {
        if (shm_table[i].used && shm_table[i].key == key)
            return &shm_table[i];
    }
    return 0;
}

static shm_segment_t* alloc_slot(void)
{
    for (int i = 0; i < SHM_MAX_SEGMENTS; i++) {
        if (!shm_table[i].used)
            return &shm_table[i];
    }
    return 0;
}

/* find free virtual range in process address space (above mmap area) */
#define SHM_VA_BASE  0xA0000000u
#define SHM_VA_LIMIT 0xB0000000u

static uint32_t find_shm_va(uint32_t* pd, uint32_t npages)
{
    uint32_t candidate = SHM_VA_BASE;

    while (candidate + npages * PAGE_SIZE <= SHM_VA_LIMIT) {
        int conflict = 0;
        for (uint32_t p = 0; p < npages; p++) {
            uint32_t va = candidate + p * PAGE_SIZE;
            uint32_t pdi = PD_INDEX(va);
            if (pd[pdi] & PAGE_PRESENT) {
                uint32_t* pt = (uint32_t*)(pd[pdi] & ~0xFFF);
                if (pt[PT_INDEX(va)] & PAGE_PRESENT) {
                    conflict = 1;
                    candidate = va + PAGE_SIZE;
                    break;
                }
            }
        }
        if (!conflict) return candidate;
    }
    return 0;
}

//public api
int shm_get(int key, uint32_t size, int flags)
{
    if (size == 0 && key == IPC_PRIVATE) {
        kprint("[SHM] shm_get: size=0 with IPC_PRIVATE\n");
        return -1;
    }

    shm_segment_t* seg = find_by_key(key);

    if (seg) {
        if ((flags & IPC_CREAT) && (flags & IPC_EXCL)) {
            kprint("[SHM] shm_get: key exists + IPC_EXCL\n");
            return -1;
        }
        kprint("[SHM] shm_get: found existing id=");
        char buf[16]; itoa(seg->id, buf); kprint(buf); kprint("\n");
        return seg->id;
    }

    if (!(flags & IPC_CREAT) && key != IPC_PRIVATE) {
        kprint("[SHM] shm_get: key not found, no IPC_CREAT\n");
        return -1;
    }

    if (size == 0) {
        kprint("[SHM] shm_get: size=0 on create\n");
        return -1;
    }

    seg = alloc_slot();
    if (!seg) {
        kprint("[SHM] shm_get: table full\n");
        return -1;
    }

    uint32_t npages = (size + PAGE_SIZE - 1) / PAGE_SIZE;
    if (npages > SHM_MAX_PAGES) {
        kprint("[SHM] shm_get: too many pages (");
        char buf2[16]; itoa((int)npages, buf2); kprint(buf2);
        kprint(" > "); itoa(SHM_MAX_PAGES, buf2); kprint(buf2);
        kprint(")\n");
        return -1;
    }

    for (uint32_t i = 0; i < npages; i++) {
        void* page = kalloc();
        if (!page) {
            kprint("[SHM] shm_get: kalloc failed at page ");
            char buf3[16]; itoa((int)i, buf3); kprint(buf3); kprint("\n");
            for (uint32_t j = 0; j < i; j++)
                kfree_page((void*)seg->phys_pages[j]);
            return -1;
        }
        uint8_t* p = (uint8_t*)page;
        for (uint32_t b = 0; b < PAGE_SIZE; b++) p[b] = 0;
        seg->phys_pages[i] = (uint32_t)page;
    }

    seg->id             = shm_next_id++;
    seg->key            = key;
    seg->size           = size;
    seg->npages         = npages;
    seg->nattach        = 0;
    seg->marked_destroy = 0;
    seg->used           = 1;
    seg->creator_pid    = current_task ? current_task->pid : 0;

    for (int i = 0; i < SHM_MAX_ATTACH; i++) {
        seg->attachments[i].pid   = 0;
        seg->attachments[i].vaddr = 0;
    }

    kprint("[SHM] shm_get: created id=");
    char b[16]; itoa(seg->id, b); kprint(b);
    kprint(" key="); itoa(seg->key, b); kprint(b);
    kprint(" size="); itoa((int)seg->size, b); kprint(b);
    kprint(" pages="); itoa((int)seg->npages, b); kprint(b);
    kprint("\n");

    return seg->id;
}

uint32_t shm_at(int shmid, uint32_t shmaddr, int flags)
{
    if (!current_task || !current_task->page_directory) {
        kprint("[SHM] shm_at: no current task\n");
        return (uint32_t)-1;
    }

    shm_segment_t* seg = find_by_id(shmid);
    if (!seg) {
        kprint("[SHM] shm_at: invalid shmid=");
        char buf[16]; itoa(shmid, buf); kprint(buf); kprint("\n");
        return (uint32_t)-1;
    }

    if (seg->nattach >= SHM_MAX_ATTACH) {
        kprint("[SHM] shm_at: max attachments reached for id=");
        char buf[16]; itoa(shmid, buf); kprint(buf); kprint("\n");
        return (uint32_t)-1;
    }

    /* find per-task slot */
    int task_slot = -1;
    for (int i = 0; i < TASK_SHM_MAX; i++) {
        if (current_task->shm_attachments[i].shm_id == 0) {
            task_slot = i;
            break;
        }
    }
    if (task_slot < 0) {
        kprint("[SHM] shm_at: task shm slots full\n");
        return (uint32_t)-1;
    }

    uint32_t* pd = current_task->page_directory;
    uint32_t va;

    if (shmaddr != 0) {
        if (shmaddr % PAGE_SIZE != 0) {
            kprint("[SHM] shm_at: unaligned shmaddr\n");
            return (uint32_t)-1;
        }
        va = shmaddr;
    } else {
        va = find_shm_va(pd, seg->npages);
        if (va == 0) {
            kprint("[SHM] shm_at: no free VA space\n");
            return (uint32_t)-1;
        }
    }

    int page_flags = PAGE_PRESENT | PAGE_USER;
    if (!(flags & SHM_RDONLY))
        page_flags |= PAGE_RW;

    for (uint32_t i = 0; i < seg->npages; i++) {
        vmm_map(pd, va + i * PAGE_SIZE, seg->phys_pages[i], page_flags);
    }

    /* record in segment */
    for (int i = 0; i < SHM_MAX_ATTACH; i++) {
        if (seg->attachments[i].pid == 0) {
            seg->attachments[i].pid   = current_task->pid;
            seg->attachments[i].vaddr = va;
            break;
        }
    }
    seg->nattach++;

    /* record in task */
    current_task->shm_attachments[task_slot].shm_id    = shmid;
    current_task->shm_attachments[task_slot].shm_vaddr = va;

    kprint("[SHM] shm_at: pid=");
    char b[16]; itoa((int)current_task->pid, b); kprint(b);
    kprint(" id="); itoa(shmid, b); kprint(b);
    kprint(" va=0x"); char hx[16]; hex_to_ascii(va, hx); kprint(hx);
    kprint(" nattach="); itoa(seg->nattach, b); kprint(b);
    kprint("\n");

    return va;
}

static void unmap_segment_from_pd(uint32_t* pd, uint32_t va, uint32_t npages)
{
    for (uint32_t i = 0; i < npages; i++) {
        uint32_t addr = va + i * PAGE_SIZE;
        uint32_t pdi = PD_INDEX(addr);
        if (!(pd[pdi] & PAGE_PRESENT)) continue;
        uint32_t* pt = (uint32_t*)(pd[pdi] & ~0xFFF);
        uint32_t pti = PT_INDEX(addr);
        /* clear PTE but do NOT free physical page — it belongs to shm */
        pt[pti] = 0;
        __asm__ __volatile__("invlpg (%0)" :: "r"(addr) : "memory");
    }
}

static void try_destroy_segment(shm_segment_t* seg)
{
    if (seg->nattach > 0 || !seg->marked_destroy) return;

    kprint("[SHM] destroying id=");
    char b[16]; itoa(seg->id, b); kprint(b);
    kprint(" ("); itoa((int)seg->npages, b); kprint(b);
    kprint(" pages freed)\n");

    for (uint32_t i = 0; i < seg->npages; i++) {
        kfree_page((void*)seg->phys_pages[i]);
        seg->phys_pages[i] = 0;
    }
    seg->used = 0;
    seg->id   = 0;
}

int shm_dt(uint32_t shmaddr)
{
    if (!current_task || !current_task->page_directory) {
        kprint("[SHM] shm_dt: no current task\n");
        return -1;
    }

    /* find task attachment */
    int task_slot = -1;
    int shmid = 0;
    for (int i = 0; i < TASK_SHM_MAX; i++) {
        if (current_task->shm_attachments[i].shm_vaddr == shmaddr &&
            current_task->shm_attachments[i].shm_id != 0) {
            task_slot = i;
            shmid = current_task->shm_attachments[i].shm_id;
            break;
        }
    }
    if (task_slot < 0) {
        kprint("[SHM] shm_dt: addr 0x");
        char hx[16]; hex_to_ascii(shmaddr, hx); kprint(hx);
        kprint(" not found in task\n");
        return -1;
    }

    shm_segment_t* seg = find_by_id(shmid);
    if (!seg) {
        kprint("[SHM] shm_dt: segment id=");
        char b[16]; itoa(shmid, b); kprint(b);
        kprint(" gone\n");
        current_task->shm_attachments[task_slot].shm_id    = 0;
        current_task->shm_attachments[task_slot].shm_vaddr = 0;
        return -1;
    }

    unmap_segment_from_pd(current_task->page_directory, shmaddr, seg->npages);

    /* remove from segment attachment list */
    for (int i = 0; i < SHM_MAX_ATTACH; i++) {
        if (seg->attachments[i].pid == current_task->pid &&
            seg->attachments[i].vaddr == shmaddr) {
            seg->attachments[i].pid   = 0;
            seg->attachments[i].vaddr = 0;
            break;
        }
    }
    if (seg->nattach > 0) seg->nattach--;

    /* clear task slot */
    current_task->shm_attachments[task_slot].shm_id    = 0;
    current_task->shm_attachments[task_slot].shm_vaddr = 0;

    kprint("[SHM] shm_dt: pid=");
    char b[16]; itoa((int)current_task->pid, b); kprint(b);
    kprint(" detached id="); itoa(shmid, b); kprint(b);
    kprint(" nattach="); itoa(seg->nattach, b); kprint(b);
    kprint("\n");

    try_destroy_segment(seg);
    return 0;
}

int shm_ctl(int shmid, int cmd, void* buf)
{
    (void)buf;

    shm_segment_t* seg = find_by_id(shmid);
    if (!seg) {
        kprint("[SHM] shm_ctl: invalid id=");
        char b[16]; itoa(shmid, b); kprint(b); kprint("\n");
        return -1;
    }

    if (cmd == IPC_RMID) {
        seg->marked_destroy = 1;
        kprint("[SHM] shm_ctl: IPC_RMID id=");
        char b2[16]; itoa(shmid, b2); kprint(b2);
        kprint(" nattach="); itoa(seg->nattach, b2); kprint(b2);
        kprint("\n");
        try_destroy_segment(seg);
        return 0;
    }

    if (cmd == IPC_STAT) {
        kprint("[SHM] shm_ctl: IPC_STAT id=");
        char b2[16]; itoa(shmid, b2); kprint(b2);
        kprint(" size="); itoa((int)seg->size, b2); kprint(b2);
        kprint(" nattach="); itoa(seg->nattach, b2); kprint(b2);
        kprint("\n");
        return 0;
    }

    kprint("[SHM] shm_ctl: unknown cmd=");
    char b2[16]; itoa(cmd, b2); kprint(b2); kprint("\n");
    return -1;
}

/* called when a process exits — detach all shm segments */
void shm_detach_all(uint32_t pid, uint32_t* pd)
{
    for (int i = 0; i < SHM_MAX_SEGMENTS; i++) {
        shm_segment_t* seg = &shm_table[i];
        if (!seg->used) continue;

        for (int j = 0; j < SHM_MAX_ATTACH; j++) {
            if (seg->attachments[j].pid != pid) continue;

            if (pd) {
                unmap_segment_from_pd(pd, seg->attachments[j].vaddr, seg->npages);
            }

            seg->attachments[j].pid   = 0;
            seg->attachments[j].vaddr = 0;
            if (seg->nattach > 0) seg->nattach--;

            kprint("[SHM] detach_all: pid=");
            char b[16]; itoa((int)pid, b); kprint(b);
            kprint(" id="); itoa(seg->id, b); kprint(b);
            kprint(" nattach="); itoa(seg->nattach, b); kprint(b);
            kprint("\n");
        }

        try_destroy_segment(seg);
    }
}

void shm_debug_dump(void)
{
    kprint("[SHM] === Shared Memory Segments ===\n");
    int count = 0;
    for (int i = 0; i < SHM_MAX_SEGMENTS; i++) {
        shm_segment_t* seg = &shm_table[i];
        if (!seg->used) continue;
        count++;
        char b[16];
        kprint("  id="); itoa(seg->id, b); kprint(b);
        kprint(" key="); itoa(seg->key, b); kprint(b);
        kprint(" size="); itoa((int)seg->size, b); kprint(b);
        kprint(" pages="); itoa((int)seg->npages, b); kprint(b);
        kprint(" nattach="); itoa(seg->nattach, b); kprint(b);
        if (seg->marked_destroy) kprint(" [RMID]");
        kprint("\n");
        for (int j = 0; j < SHM_MAX_ATTACH; j++) {
            if (seg->attachments[j].pid == 0) continue;
            kprint("    pid="); itoa((int)seg->attachments[j].pid, b); kprint(b);
            kprint(" va=0x"); char hx[16]; hex_to_ascii(seg->attachments[j].vaddr, hx); kprint(hx);
            kprint("\n");
        }
    }
    if (count == 0) kprint("  (none)\n");
}