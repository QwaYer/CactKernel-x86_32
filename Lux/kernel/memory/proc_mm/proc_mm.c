#include "proc_mm.h"
#include "memory.h"
#include "kernel.h"

void proc_free_pages(proc_page_tracker_t* t)
{
    if (!t) return;

    for (uint32_t i = 0; i < t->count; i++) {
        if (t->pages[i]) {
            kfree_page(t->pages[i]);
            t->pages[i] = 0;
        }
    }
    t->count = 0;

    if (t->page_dir) {
        vmm_free_address_space(t->page_dir);
        t->page_dir = 0;
    }
}