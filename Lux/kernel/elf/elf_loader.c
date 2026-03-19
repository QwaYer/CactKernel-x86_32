#include "elf.h"
#include "dynlink.h"
#include "kernel.h"
#include "vfs.h"
#include "memory.h"
#include "page_fault.h"
#include "proc_mm.h"
#include "libc.h"


void* load_elf(char* path, uint32_t* pd, proc_page_tracker_t* tracker)
{
    tracker->page_dir = pd;

    // [DBG]
    kprint("[ELF] loading: "); kprint(path); kprint("\n");

    vfs_node_t *base = (path[0] == '/') ? vfs_root : vfs_root;
    struct vfs_node* file = vfs_walk_path(base, path);
    if (!file) {
        kprint("[ELF] ERR: file not found: "); kprint(path); kprint("\n");
        return 0;
    }

    Elf32_Ehdr hdr;
    if (read_vfs(file, 0, sizeof(Elf32_Ehdr), (char*)&hdr) <= 0) {
        kprint("[ELF] ERR: cannot read header\n");
        return 0;
    }

    if (*(uint32_t*)hdr.e_ident != ELF_MAGIC) {
        kprint("[ELF] ERR: bad magic\n");
        return 0;
    }
    if (hdr.e_machine != 3) {
        kprint("[ELF] ERR: not i386\n");
        return 0;
    }

    // [DBG]
    kprint("[ELF] entry=0x"); kprint_hex(hdr.e_entry);
    kprint(" phnum="); char buf[16]; itoa(hdr.e_phnum, buf); kprint(buf); kprint("\n");

    for (int i = 0; i < hdr.e_phnum; i++) {
        Elf32_Phdr ph;
        if (read_vfs(file,
                     hdr.e_phoff + (uint32_t)i * hdr.e_phentsize,
                     sizeof(Elf32_Phdr),
                     (char*)&ph) <= 0) {
            kprint("[ELF] ERR: cannot read phdr\n");
            proc_free_pages(tracker);
            return 0;
        }

        if (ph.p_type != PT_LOAD || ph.p_memsz == 0) continue;

        // [DBG]
        kprint("[ELF] PT_LOAD vaddr=0x"); kprint_hex(ph.p_vaddr);
        kprint(" filesz=0x"); kprint_hex(ph.p_filesz);
        kprint(" memsz=0x"); kprint_hex(ph.p_memsz); kprint("\n");

        // page-aligned start/end
        uint32_t seg_start = ph.p_vaddr & ~0xFFFu;
        uint32_t seg_end   = (ph.p_vaddr + ph.p_memsz + 0xFFF) & ~0xFFFu;
        uint32_t file_end  = ph.p_vaddr + ph.p_filesz;

        for (uint32_t va = seg_start; va < seg_end; va += PAGE_SIZE) {
            // check if this page has any file data
            if (va < file_end) {
                // page with file data (possibly partial)
                void* phys = kalloc();
                if (!phys) {
                    kprint("[ELF] ERR: OOM at va=0x"); kprint_hex(va); kprint("\n");
                    proc_free_pages(tracker);
                    return 0;
                }
                if (proc_tracker_add(tracker, phys) < 0) {
                    kprint("[ELF] ERR: tracker add failed\n");
                    kfree_page(phys);
                    proc_free_pages(tracker);
                    return 0;
                }

                // zero entire page first
                uint8_t* p = (uint8_t*)phys;
                for (int k = 0; k < (int)PAGE_SIZE; k++) p[k] = 0;

                // compute file data range within this page
                uint32_t copy_start = (va > ph.p_vaddr) ? va : ph.p_vaddr;
                uint32_t copy_end   = ((va + PAGE_SIZE) < file_end) ? (va + PAGE_SIZE) : file_end;

                if (copy_end > copy_start) {
                    uint32_t page_offset = copy_start - va;
                    uint32_t file_offset = ph.p_offset + (copy_start - ph.p_vaddr);
                    uint32_t copy_sz     = copy_end - copy_start;

                    if (read_vfs(file, file_offset, copy_sz, (char*)phys + page_offset) <= 0) {
                        kprint("[ELF] ERR: read failed at offset 0x"); kprint_hex(file_offset); kprint("\n");
                        proc_free_pages(tracker);
                        return 0;
                    }
                }

                vmm_map(pd, va, (uint32_t)phys, PAGE_USER | PAGE_RW | PAGE_PRESENT);
            } else {
                // pure BSS — use demand paging (zero-fill on fault)
                if (vmm_map_zero(pd, va, PAGE_SIZE, PAGE_USER) != 0) {
                    kprint("[ELF] ERR: vmm_map_zero failed at 0x"); kprint_hex(va); kprint("\n");
                    proc_free_pages(tracker);
                    return 0;
                }
            }
        }
    }

    // [DBG]
    kprint("[ELF] loaded ok, total pages=");
    itoa((int)tracker->count, buf); kprint(buf); kprint("\n");

    return (void*)hdr.e_entry;
}
// Append to elf_loader.c — load_elf_dynamic function

void* load_elf_dynamic(char*                path,
                        uint32_t*            pd,
                        proc_page_tracker_t* tracker,
                        dyn_ctx_t*           ctx)
{
    tracker->page_dir = pd;

    // [DBG]
    kprint("[ELF-DYN] loading: "); kprint(path); kprint("\n");

    vfs_node_t *base = (path[0] == '/') ? vfs_root : vfs_root;
    struct vfs_node* file = vfs_walk_path(base, path);
    if (!file) {
        kprint("[ELF-DYN] ERR: file not found: "); kprint(path); kprint("\n");
        return 0;
    }

    Elf32_Ehdr hdr;
    if (read_vfs(file, 0, sizeof(Elf32_Ehdr), (char*)&hdr) <= 0) {
        kprint("[ELF-DYN] ERR: cannot read header\n");
        return 0;
    }
    if (*(uint32_t*)hdr.e_ident != ELF_MAGIC) {
        kprint("[ELF-DYN] ERR: bad magic\n");
        return 0;
    }
    if (hdr.e_machine != 3) {
        kprint("[ELF-DYN] ERR: not i386\n");
        return 0;
    }

    Elf32_Dyn* dyn_vaddr = 0;
    uint32_t   dyn_size  = 0;

    // [DBG]
    char buf[16];
    kprint("[ELF-DYN] entry=0x"); kprint_hex(hdr.e_entry);
    kprint(" phnum="); itoa(hdr.e_phnum, buf); kprint(buf); kprint("\n");

    for (int i = 0; i < hdr.e_phnum; i++) {
        Elf32_Phdr ph;
        if (read_vfs(file,
                     hdr.e_phoff + (uint32_t)i * hdr.e_phentsize,
                     sizeof(Elf32_Phdr),
                     (char*)&ph) <= 0) {
            kprint("[ELF-DYN] ERR: cannot read phdr\n");
            proc_free_pages(tracker);
            return 0;
        }

        if (ph.p_type == PT_LOAD && ph.p_memsz > 0) {
            // [DBG]
            kprint("[ELF-DYN] PT_LOAD vaddr=0x"); kprint_hex(ph.p_vaddr);
            kprint(" filesz=0x"); kprint_hex(ph.p_filesz);
            kprint(" memsz=0x"); kprint_hex(ph.p_memsz); kprint("\n");

            uint32_t seg_start = ph.p_vaddr & ~0xFFFu;
            uint32_t seg_end   = (ph.p_vaddr + ph.p_memsz + 0xFFF) & ~0xFFFu;
            uint32_t file_end  = ph.p_vaddr + ph.p_filesz;

            for (uint32_t va = seg_start; va < seg_end; va += PAGE_SIZE) {
                if (va < file_end) {
                    void* phys = kalloc();
                    if (!phys) {
                        kprint("[ELF-DYN] ERR: OOM\n");
                        proc_free_pages(tracker);
                        return 0;
                    }
                    if (proc_tracker_add(tracker, phys) < 0) {
                        kprint("[ELF-DYN] ERR: tracker add failed\n");
                        kfree_page(phys);
                        proc_free_pages(tracker);
                        return 0;
                    }

                    uint8_t* p = (uint8_t*)phys;
                    for (int k = 0; k < (int)PAGE_SIZE; k++) p[k] = 0;

                    uint32_t copy_start = (va > ph.p_vaddr) ? va : ph.p_vaddr;
                    uint32_t copy_end   = ((va + PAGE_SIZE) < file_end) ? (va + PAGE_SIZE) : file_end;

                    if (copy_end > copy_start) {
                        uint32_t page_offset = copy_start - va;
                        uint32_t file_offset = ph.p_offset + (copy_start - ph.p_vaddr);
                        uint32_t copy_sz     = copy_end - copy_start;

                        if (read_vfs(file, file_offset, copy_sz, (char*)phys + page_offset) <= 0) {
                            kprint("[ELF-DYN] ERR: read failed\n");
                            proc_free_pages(tracker);
                            return 0;
                        }
                    }

                    vmm_map(pd, va, (uint32_t)phys, PAGE_USER | PAGE_RW | PAGE_PRESENT);
                } else {
                    if (vmm_map_zero(pd, va, PAGE_SIZE, PAGE_USER) != 0) {
                        kprint("[ELF-DYN] ERR: vmm_map_zero failed\n");
                        proc_free_pages(tracker);
                        return 0;
                    }
                }
            }
        }

        if (ph.p_type == PT_DYNAMIC) {
            dyn_vaddr = (Elf32_Dyn*)ph.p_vaddr;
            dyn_size  = ph.p_filesz;
        }
    }

    if (dyn_vaddr) {
        dynlink_ctx_init(ctx, pd, tracker);

        uint32_t load_base = 0;

        int rc = dynlink_process_dynamic(ctx, load_base, dyn_vaddr);
        if (rc != 0) {
            kprint("[ELF-DYN] ERR: dynamic linking failed\n");
            proc_free_pages(tracker);
            return 0;
        }

        kprint("[ELF-DYN] dynamic linking complete\n"); // [DBG]
    } else {
        kprint("[ELF-DYN] no PT_DYNAMIC — treating as static\n"); // [DBG]
    }

    // [DBG]
    kprint("[ELF-DYN] loaded ok, total pages=");
    itoa((int)tracker->count, buf); kprint(buf); kprint("\n");

    return (void*)hdr.e_entry;
}