#include "proc_mm.h"
#include "memory.h"
#include "kernel.h"
#include "libc.h"

int proc_tracker_add(proc_page_tracker_t* t, void* phys)
{
    if (!t || !phys) return -1;

    if (!t->pages) {
        t->pages = (void**)kmalloc(PROC_INITIAL_PAGES * sizeof(void*));
        if (!t->pages) {
            kprint("[PROC_MM] ERR: initial alloc failed\n");
            return -1;
        }
        for (uint32_t i = 0; i < PROC_INITIAL_PAGES; i++)
            t->pages[i] = 0;
        t->capacity = PROC_INITIAL_PAGES;
    }

    if (t->count >= t->capacity) {
        uint32_t new_cap = t->capacity + PROC_GROW_STEP;

        void** new_arr = (void**)kmalloc(new_cap * sizeof(void*));
        if (!new_arr) {
            kprint("[PROC_MM] ERR: grow alloc failed\n");
            return -1;
        }
        for (uint32_t i = 0; i < t->count; i++)
            new_arr[i] = t->pages[i];
        for (uint32_t i = t->count; i < new_cap; i++)
            new_arr[i] = 0;

        kfree_heap(t->pages);
        t->pages    = new_arr;
        t->capacity = new_cap;
    }

    t->pages[t->count++] = phys;
    return 0;
}

void proc_free_pages(proc_page_tracker_t* t)
{
    if (!t) return;

    if (t->pages) {
        for (uint32_t i = 0; i < t->count; i++) {
            if (t->pages[i]) {
                kfree_page(t->pages[i]);
                t->pages[i] = 0;
            }
        }
        kfree_heap(t->pages);
    }

    t->pages    = 0;
    t->count    = 0;
    t->capacity = 0;

    if (t->page_dir) {
        vmm_free_address_space(t->page_dir);
        t->page_dir = 0;
    }
}