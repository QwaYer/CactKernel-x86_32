#include "elf.h"
#include "dynlink.h"
#include "kernel.h"
#include "vfs.h"
#include "memory.h"
#include "proc_mm.h"
#include "klib.h"


void* load_elf(char* path, uint32_t* pd, proc_page_tracker_t* tracker)
{
    tracker->page_dir = pd;

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

    kprint("[ELF] e_entry=0x");
    {
        char ebuf[12];
        hex_to_ascii(hdr.e_entry, ebuf);
        kprint(ebuf);
    }
    kprint("\n");

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
    uint32_t   dyn_size  = 0;

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
    }

    return (void*)hdr.e_entry;
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