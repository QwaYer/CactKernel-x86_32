#include "elf.h"
#include "dynlink.h"
#include "kernel.h"
#include "vfs.h"
#include "memory.h"
#include "proc_mm.h"
#include "klib.h"

int elf_is_dynamic(char* path) {
    if (!path) return 0;
    vfs_node_t *base = (path[0] == '/') ? vfs_root : vfs_root;
    struct vfs_node* file = vfs_walk_path(base, path);
    if (!file) return 0;

    Elf32_Ehdr hdr;
    if (read_vfs(file, 0, sizeof(Elf32_Ehdr), (char*)&hdr) <= 0) return 0;
    if (*(uint32_t*)hdr.e_ident != ELF_MAGIC) return 0;
    if (hdr.e_machine != 3) return 0;
    if (hdr.e_type == 3) return 1; // ET_DYN

    for (int i = 0; i < hdr.e_phnum; i++) {
        Elf32_Phdr ph;
        if (read_vfs(file,
                     hdr.e_phoff + (uint32_t)i * hdr.e_phentsize,
                     sizeof(Elf32_Phdr),
                     (char*)&ph) <= 0) {
            return 0;
        }
        if (ph.p_type == PT_DYNAMIC) return 1;
    }
    return 0;
}


void* load_elf(char* path, uint32_t* pd, proc_page_tracker_t* tracker)
{
    tracker->page_dir = pd;

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

        uint32_t seg_start = ph.p_vaddr & ~0xFFFu;
        uint32_t seg_end   = (ph.p_vaddr + ph.p_memsz + 0xFFF) & ~0xFFFu;
        uint32_t file_end  = ph.p_vaddr + ph.p_filesz;

        for (uint32_t va = seg_start; va < seg_end; va += PAGE_SIZE) {
            if (va < file_end) {
                void* phys = kalloc();
                if (!phys) {
                    kprint("[ELF] ERR: OOM\n");
                    proc_free_pages(tracker);
                    return 0;
                }
                if (proc_tracker_add(tracker, phys) < 0) {
                    kprint("[ELF] ERR: tracker_add failed\n");
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

                    int rd = read_vfs(file, file_offset, copy_sz, (char*)phys + page_offset);
                    if (rd <= 0) {
                        kprint("[ELF] ERR: read_vfs failed at off=");
                        char _b[12]; itoa(file_offset, _b); kprint(_b);
                        kprint(" sz="); itoa(copy_sz, _b); kprint(_b);
                        kprint(" ret="); itoa(rd, _b); kprint(_b);
                        kprint("\n");
                        proc_free_pages(tracker);
                        return 0;
                    }
                }

                vmm_map(pd, va, (uint32_t)phys, PAGE_USER | PAGE_RW | PAGE_PRESENT);
            } else {
                /* p_memsz > p_filesz: .bss / zero tail — need present mappings (not demand-only). */
                void* zphys = kalloc();
                if (!zphys) {
                    kprint("[ELF] ERR: OOM (bss)\n");
                    proc_free_pages(tracker);
                    return 0;
                }
                if (proc_tracker_add(tracker, zphys) < 0) {
                    kprint("[ELF] ERR: tracker_add failed (bss)\n");
                    kfree_page(zphys);
                    proc_free_pages(tracker);
                    return 0;
                }
                uint8_t* zp = (uint8_t*)zphys;
                for (int k = 0; k < (int)PAGE_SIZE; k++) zp[k] = 0;
                vmm_map(pd, va, (uint32_t)zphys, PAGE_USER | PAGE_RW | PAGE_PRESENT);
            }
        }
    }

    return (void*)hdr.e_entry;
}

void* load_elf_dynamic(char*                path,
                        uint32_t*            pd,
                        proc_page_tracker_t* tracker,
                        dyn_ctx_t*           ctx)
{
    tracker->page_dir = pd;

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
    uint32_t   min_load_vaddr = 0xFFFFFFFFu;
    uint32_t   max_load_vaddr = 0;
    uint32_t   load_bias = 0;

    for (int i = 0; i < hdr.e_phnum; i++) {
        Elf32_Phdr ph;
        if (read_vfs(file,
                     hdr.e_phoff + (uint32_t)i * hdr.e_phentsize,
                     sizeof(Elf32_Phdr),
                     (char*)&ph) <= 0) {
            kprint("[ELF-DYN] ERR: cannot read phdr (scan)\n");
            return 0;
        }
        if (ph.p_type != PT_LOAD || ph.p_memsz == 0) continue;
        uint32_t seg_start = ph.p_vaddr & ~0xFFFu;
        uint32_t seg_end   = (ph.p_vaddr + ph.p_memsz + 0xFFFu) & ~0xFFFu;
        if (seg_start < min_load_vaddr) min_load_vaddr = seg_start;
        if (seg_end > max_load_vaddr) max_load_vaddr = seg_end;
    }

    if (min_load_vaddr == 0xFFFFFFFFu) {
        kprint("[ELF-DYN] ERR: no PT_LOAD segments\n");
        return 0;
    }

    // Current policy maps to requested virtual addresses; keep explicit bias for correctness.
    load_bias = min_load_vaddr - min_load_vaddr;
    uint32_t runtime_base = min_load_vaddr + load_bias;

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
            uint32_t seg_start = (ph.p_vaddr + load_bias) & ~0xFFFu;
            uint32_t seg_end   = (ph.p_vaddr + load_bias + ph.p_memsz + 0xFFF) & ~0xFFFu;
            uint32_t file_end  = ph.p_vaddr + ph.p_filesz;

            for (uint32_t va = seg_start; va < seg_end; va += PAGE_SIZE) {
                uint32_t orig_va = va - load_bias;
                if (orig_va < file_end) {
                    void* phys = kalloc();
                    if (!phys) {
                        kprint("[ELF-DYN] ERR: OOM\n");
                        proc_free_pages(tracker);
                        return 0;
                    }
                    if (proc_tracker_add(tracker, phys) < 0) {
                        kfree_page(phys);
                        proc_free_pages(tracker);
                        return 0;
                    }

                    uint8_t* p = (uint8_t*)phys;
                    for (int k = 0; k < (int)PAGE_SIZE; k++) p[k] = 0;

                    uint32_t copy_start = (orig_va > ph.p_vaddr) ? orig_va : ph.p_vaddr;
                    uint32_t copy_end   = ((orig_va + PAGE_SIZE) < file_end) ? (orig_va + PAGE_SIZE) : file_end;

                    if (copy_end > copy_start) {
                        uint32_t page_offset = copy_start - orig_va;
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
                    void* zphys = kalloc();
                    if (!zphys) {
                        kprint("[ELF-DYN] ERR: OOM (bss)\n");
                        proc_free_pages(tracker);
                        return 0;
                    }
                    if (proc_tracker_add(tracker, zphys) < 0) {
                        kfree_page(zphys);
                        proc_free_pages(tracker);
                        return 0;
                    }
                    uint8_t* zp = (uint8_t*)zphys;
                    for (int k = 0; k < (int)PAGE_SIZE; k++) zp[k] = 0;
                    vmm_map(pd, va, (uint32_t)zphys, PAGE_USER | PAGE_RW | PAGE_PRESENT);
                }
            }
        }

        if (ph.p_type == PT_DYNAMIC) {
            /* В CactOS-policy все исполняемые ELF (и ET_EXEC PIE, и ET_DYN с
             * фиксированной базой) маппятся ровно на свои p_vaddr — bias = 0,
             * адреса в .dynamic уже абсолютные. */
            dyn_vaddr = (Elf32_Dyn*)ph.p_vaddr;
        }
    }

    if (dyn_vaddr) {
        dynlink_ctx_init(ctx, pd, tracker);
        /* image_start используется только как идентификатор «который это so»
         * в _find_loaded_*. У главного бинаря его роль играет адрес .symtab,
         * поэтому достаточно нулей. sym_bias = 0 — st_value уже абсолютные. */
        uint32_t image_start = 0;
        uint32_t sym_bias    = 0;
        /* Image is mapped only in pd; task_exec switches CR3 later. Dereferencing
         * dyn_vaddr must run while CR3 == pd or we read the wrong address space. */
        uint32_t* saved_pd = get_current_pd();
        switch_paging(pd);

        int rc = dynlink_process_dynamic(ctx, image_start, sym_bias, dyn_vaddr);
        if (rc != 0) {
            switch_paging(saved_pd);
            kprint("[ELF-DYN] ERR: dynamic linking failed\n");
            proc_free_pages(tracker);
            return 0;
        }
        {
            Elf32_Dyn* test = dyn_vaddr;
            if (test->d_tag == 0 || test->d_tag > 100) {
                kprint("[ELF-DYN] ERR: bad dynamic section!\n");
            }
        }

        switch_paging(saved_pd);
    }

    return (void*)(hdr.e_entry + load_bias);
}

uint32_t elf_get_brk_start(struct vfs_node* file) {
    if (!file) return 0;
 
    Elf32_Ehdr hdr;
    if (read_vfs(file, 0, sizeof(Elf32_Ehdr), (char*)&hdr) <= 0) return 0;
    if (*(uint32_t*)hdr.e_ident != ELF_MAGIC) return 0;
 
    uint32_t highest = 0;
    for (int i = 0; i < hdr.e_phnum; i++) {
        Elf32_Phdr ph;
        if (read_vfs(file,
                     hdr.e_phoff + (uint32_t)i * hdr.e_phentsize,
                     sizeof(Elf32_Phdr), (char*)&ph) <= 0)
            break;
        if (ph.p_type != PT_LOAD) continue;
        uint32_t end = ph.p_vaddr + ph.p_memsz;
        if (end > highest) highest = end;
    }
 
    return (highest + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
}