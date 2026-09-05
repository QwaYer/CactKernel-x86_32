#include "elf.h"
#include "kernel.h"
#include "vfs.h"
#include "memory.h"
#include "proc_mm.h"
#include "klib.h"

/* PT_INTERP (userspace ld.so) handoff data. main_phdr points at a mapped copy
 * of the program header table (allocated by _map_image when the phdrs are not
 * covered by a PT_LOAD), so ld.so can scan it in user space just like Linux. */
typedef struct {
    uint32_t main_entry;
    uint32_t main_base;
    uint32_t main_phdr;
    uint32_t main_phnum;
    uint32_t interp_base;
} elf_interp_info_t;

/* Loads only the PT_LOAD segments (no relocation/dynlink pass) for a file.
 * out_phdr is the user-space address of a mapped program-header table
 * (0 when the phdrs are already covered by a segment mapping). */
static int _map_image(struct vfs_node* file, uint32_t* pd,
                      proc_page_tracker_t* tracker,
                      uint32_t* out_entry, uint32_t* out_min_vaddr,
                      uint32_t* out_phdr, uint32_t* out_phnum)
{
    Elf32_Ehdr hdr;
    if (read_vfs(file, 0, sizeof(Elf32_Ehdr), (char*)&hdr) <= 0) return -1;
    if (*(uint32_t*)hdr.e_ident != ELF_MAGIC) return -1;
    if (hdr.e_ident[EI_CLASS] != ELFCLASS32) return -1;
    if (hdr.e_ident[EI_DATA]  != ELFDATA2LSB) return -1;
    if (hdr.e_ident[EI_VERSION] != EV_CURRENT) return -1;
    if (hdr.e_machine != 3) return -1;
    if (hdr.e_phentsize < sizeof(Elf32_Phdr)) return -1;
    if ((uint32_t)hdr.e_phnum > 65535u) return -1;
    uint64_t ph_end_check = (uint64_t)hdr.e_phoff +
                            (uint64_t)hdr.e_phnum * hdr.e_phentsize;
    if (ph_end_check > file->size) return -1;

    if (out_entry)   *out_entry   = hdr.e_entry;
    if (out_phnum)   *out_phnum   = hdr.e_phnum;
    if (out_phdr)    *out_phdr    = 0;
    if (out_min_vaddr) *out_min_vaddr = 0;

    uint32_t min_vaddr = 0xFFFFFFFFu;
    for (int i = 0; i < hdr.e_phnum; i++) {
        Elf32_Phdr ph;
        if (read_vfs(file, hdr.e_phoff + (uint64_t)i * hdr.e_phentsize,
                     sizeof(Elf32_Phdr), (char*)&ph) <= 0) return -1;
        if (ph.p_type != PT_LOAD || ph.p_memsz == 0) continue;
        uint32_t page_start = ph.p_vaddr & ~0xFFFu;
        if (page_start < min_vaddr) min_vaddr = page_start;
    }
    if (min_vaddr == 0xFFFFFFFFu) return -1;

    uint32_t phdr_bytes = (uint32_t)(ph_end_check - (uint64_t)hdr.e_phoff);
    int phdr_covered = 0;
    uint32_t phdr_vaddr = 0;

    for (int i = 0; i < hdr.e_phnum; i++) {
        Elf32_Phdr ph;
        if (read_vfs(file, hdr.e_phoff + (uint64_t)i * hdr.e_phentsize,
                     sizeof(Elf32_Phdr), (char*)&ph) <= 0) return -1;

        if (ph.p_type == PT_LOAD && ph.p_memsz > 0) {
            if (ph.p_vaddr + ph.p_memsz < ph.p_vaddr) continue;
            if (ph.p_vaddr + ph.p_filesz < ph.p_vaddr) continue;

            uint32_t seg_start = ph.p_vaddr & ~0xFFFu;
            uint32_t seg_end   = (ph.p_vaddr + ph.p_memsz + 0xFFF) & ~0xFFFu;
            uint32_t file_end  = ph.p_vaddr + ph.p_filesz;

            for (uint32_t va = seg_start; va < seg_end; va += PAGE_SIZE) {
                void* phys = kalloc();
                if (!phys) { printk("[ELF-I] ERR: OOM\n"); return -1; }
                if (proc_tracker_add(tracker, phys) < 0) {
                    free_page(phys); return -1;
                }
                uint8_t* p = (uint8_t*)phys;
                for (int k = 0; k < (int)PAGE_SIZE; k++) p[k] = 0;

                if (va < file_end) {
                    uint32_t copy_start = (va > ph.p_vaddr) ? va : ph.p_vaddr;
                    uint32_t copy_end   = ((va + PAGE_SIZE) < file_end)
                                          ? (va + PAGE_SIZE) : file_end;
                    if (copy_end > copy_start) {
                        uint32_t page_offset = copy_start - va;
                        uint32_t file_offset = ph.p_offset + (copy_start - ph.p_vaddr);
                        if (read_vfs(file, file_offset, copy_end - copy_start,
                                     (char*)phys + page_offset) <= 0) return -1;
                    }
                }
                vmm_map(pd, va, (uint32_t)phys,
                        PAGE_USER | PAGE_RW | PAGE_PRESENT);
            }

            if (!phdr_covered &&
                hdr.e_phoff >= (uint64_t)ph.p_offset &&
                (uint64_t)hdr.e_phoff + phdr_bytes <=
                    (uint64_t)ph.p_offset + ph.p_filesz) {
                phdr_covered = 1;
                phdr_vaddr   = ph.p_vaddr + (uint32_t)(hdr.e_phoff - ph.p_offset);
            }
        }
    }

    if (!phdr_covered) {
        /* Program headers live outside every PT_LOAD (p_offset of the first
         * LOAD is 0x1000 in our fixed-address images). Map one page holding a
         * copy of the phdr table just below the lowest segment so user-space
         * ld.so can read it through AT_PHDR. */
        uint32_t page_off = (uint32_t)hdr.e_phoff & 0xFFFu;
        uint32_t copy_sz  = phdr_bytes;
        if (copy_sz > (uint32_t)(PAGE_SIZE - page_off))
            copy_sz = PAGE_SIZE - page_off;

        void* phys = kalloc();
        if (!phys) { printk("[ELF-I] ERR: OOM (phdr page)\n"); return -1; }
        if (proc_tracker_add(tracker, phys) < 0) { free_page(phys); return -1; }
        uint8_t* p = (uint8_t*)phys;
        for (int k = 0; k < (int)PAGE_SIZE; k++) p[k] = 0;
        if (copy_sz > 0)
            read_vfs(file, hdr.e_phoff, copy_sz, (char*)phys + page_off);

        uint32_t page_va = min_vaddr - 0x1000u;
        vmm_map(pd, page_va, (uint32_t)phys, PAGE_USER | PAGE_RW | PAGE_PRESENT);
        phdr_vaddr = page_va + page_off;
    }

    if (out_min_vaddr) *out_min_vaddr = min_vaddr;
    if (out_phdr)      *out_phdr      = phdr_vaddr;
    return 0;
}

int elf_get_interp_path(const char* path, char* out, int out_max);
void* load_elf_interp(char* path, char* interp_path, uint32_t* pd,
                      proc_page_tracker_t* tracker, elf_interp_info_t* info);

void* load_elf(char* path, uint32_t* pd, proc_page_tracker_t* tracker)
{
    tracker->page_dir = pd;

    vfs_node_t *base = (path[0] == '/') ? vfs_root : vfs_root;
    struct vfs_node* file = vfs_walk_path(base, path);
    if (!file) {
        printk("[ELF] ERR: file not found: "); printk(path); printk("\n");
        return 0;
    }

    Elf32_Ehdr hdr;
    if (read_vfs(file, 0, sizeof(Elf32_Ehdr), (char*)&hdr) <= 0) {
        printk("[ELF] ERR: cannot read header\n");
        return 0;
    }

    if (*(uint32_t*)hdr.e_ident != ELF_MAGIC) {
        printk("[ELF] ERR: bad magic\n");
        return 0;
    }
    if (hdr.e_ident[EI_CLASS] != ELFCLASS32) {
        printk("[ELF] ERR: not 32-bit\n");
        return 0;
    }
    if (hdr.e_ident[EI_DATA] != ELFDATA2LSB) {
        printk("[ELF] ERR: not little-endian\n");
        return 0;
    }
    if (hdr.e_ident[EI_VERSION] != EV_CURRENT) {
        printk("[ELF] ERR: bad ident version\n");
        return 0;
    }
    if (hdr.e_machine != 3) {
        printk("[ELF] ERR: not i386\n");
        return 0;
    }
    if (hdr.e_phentsize < sizeof(Elf32_Phdr)) {
        printk("[ELF] ERR: phentsize too small\n");
        return 0;
    }
    if ((uint32_t)hdr.e_phnum > 65535u) {
        printk("[ELF] ERR: too many program headers\n");
        return 0;
    }
    uint64_t ph_end_check = (uint64_t)hdr.e_phoff + (uint64_t)hdr.e_phnum * hdr.e_phentsize;
    if (ph_end_check > file->size) {
        printk("[ELF] ERR: program headers overflow file\n");
        return 0;
    }

    for (int i = 0; i < hdr.e_phnum; i++) {
        Elf32_Phdr ph;
        if (read_vfs(file,
                     hdr.e_phoff + (uint64_t)i * hdr.e_phentsize,
                     sizeof(Elf32_Phdr),
                     (char*)&ph) <= 0) {
            printk("[ELF] ERR: cannot read phdr\n");
            proc_free_pages(tracker);
            return 0;
        }

        if (ph.p_type != PT_LOAD || ph.p_memsz == 0) continue;

        if (ph.p_vaddr + ph.p_memsz < ph.p_vaddr) continue;
        if (ph.p_vaddr + ph.p_filesz < ph.p_vaddr) continue;

        uint32_t seg_start = ph.p_vaddr & ~0xFFFu;
        uint32_t seg_end   = (ph.p_vaddr + ph.p_memsz + 0xFFF) & ~0xFFFu;
        uint32_t file_end  = ph.p_vaddr + ph.p_filesz;

        for (uint32_t va = seg_start; va < seg_end; va += PAGE_SIZE) {
            if (va < file_end) {
                void* phys = kalloc();
                if (!phys) {
                    printk("[ELF] ERR: OOM\n");
                    proc_free_pages(tracker);
                    return 0;
                }
                if (proc_tracker_add(tracker, phys) < 0) {
                    printk("[ELF] ERR: tracker_add failed\n");
                    free_page(phys);
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
                        printk("[ELF] ERR: read_vfs failed at off=%d sz=%d ret=%d\n",
                               (int)file_offset, (int)copy_sz, (int)rd);
                        proc_free_pages(tracker);
                        return 0;
                    }
                }

                vmm_map(pd, va, (uint32_t)phys, PAGE_USER | PAGE_RW | PAGE_PRESENT);
            } else {
                /* p_memsz > p_filesz: .bss / zero tail — need present mappings (not demand-only). */
                void* zphys = kalloc();
                if (!zphys) {
                    printk("[ELF] ERR: OOM (bss)\n");
                    proc_free_pages(tracker);
                    return 0;
                }
                if (proc_tracker_add(tracker, zphys) < 0) {
                    printk("[ELF] ERR: tracker_add failed (bss)\n");
                    free_page(zphys);
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

int elf_get_interp_path(const char* path, char* out, int out_max)
{
    if (!path || !out || out_max <= 0) return -1;

    vfs_node_t* file = vfs_walk_path(vfs_root, (char*)path);
    if (!file) return -1;

    Elf32_Ehdr hdr;
    if (read_vfs(file, 0, sizeof(Elf32_Ehdr), (char*)&hdr) <= 0) return -1;
    if (*(uint32_t*)hdr.e_ident != ELF_MAGIC) return -1;
    if (hdr.e_ident[EI_CLASS] != ELFCLASS32) return -1;
    if (hdr.e_ident[EI_DATA]  != ELFDATA2LSB) return -1;
    if (hdr.e_machine != 3) return -1;
    if (hdr.e_phentsize < sizeof(Elf32_Phdr)) return -1;
    if ((uint32_t)hdr.e_phnum > 65535u) return -1;
    uint64_t ph_end_check = (uint64_t)hdr.e_phoff +
                            (uint64_t)hdr.e_phnum * hdr.e_phentsize;
    if (ph_end_check > file->size) return -1;

    for (int i = 0; i < hdr.e_phnum; i++) {
        Elf32_Phdr ph;
        if (read_vfs(file, hdr.e_phoff + (uint64_t)i * hdr.e_phentsize,
                     sizeof(Elf32_Phdr), (char*)&ph) <= 0)
            break;
        if (ph.p_type != PT_INTERP) continue;
        if (ph.p_filesz == 0 || ph.p_filesz > (uint32_t)out_max) return -1;

        int n = read_vfs(file, ph.p_offset, ph.p_filesz, out);
        if (n <= 0) return -1;
        if (n >= out_max) n = out_max - 1;
        out[n] = '\0';
        return n;
    }
    return -1;
}

void* load_elf_interp(char* path, char* interp_path, uint32_t* pd,
                      proc_page_tracker_t* tracker, elf_interp_info_t* info)
{
    if (!path || !interp_path || !pd || !tracker || !info) return 0;
    memset(info, 0, sizeof(*info));

    vfs_node_t* main_file = vfs_walk_path(vfs_root, path);
    if (!main_file) {
        printk("[ELF-I] main not found: "); printk(path); printk("\n");
        return 0;
    }
    vfs_node_t* interp_file = vfs_walk_path(vfs_root, interp_path);
    if (!interp_file) {
        printk("[ELF-I] interpreter not found: "); printk(interp_path); printk("\n");
        return 0;
    }

    uint32_t m_entry = 0, m_min = 0, m_phdr = 0, m_phnum = 0;
    if (_map_image(main_file, pd, tracker, &m_entry, &m_min, &m_phdr, &m_phnum) != 0) {
        printk("[ELF-I] failed to map main image: "); printk(path); printk("\n");
        proc_free_pages(tracker);
        return 0;
    }
    info->main_entry = m_entry;
    info->main_base  = m_min;
    info->main_phdr  = m_phdr;
    info->main_phnum = m_phnum;

    uint32_t i_entry = 0, i_min = 0, i_phdr = 0, i_phnum = 0;
    if (_map_image(interp_file, pd, tracker, &i_entry, &i_min, &i_phdr, &i_phnum) != 0) {
        printk("[ELF-I] failed to map interpreter: "); printk(interp_path); printk("\n");
        proc_free_pages(tracker);
        return 0;
    }
    info->interp_base = i_min;

    return (void*)i_entry;
}

uint32_t elf_get_brk_start(struct vfs_node* file) {
    if (!file) return 0;
 
    Elf32_Ehdr hdr;
    if (read_vfs(file, 0, sizeof(Elf32_Ehdr), (char*)&hdr) <= 0) return 0;
    if (*(uint32_t*)hdr.e_ident != ELF_MAGIC) return 0;
    if (hdr.e_phentsize < sizeof(Elf32_Phdr)) return 0;
    if ((uint32_t)hdr.e_phnum > 65535u) return 0;
    uint64_t ph_end_check = (uint64_t)hdr.e_phoff + (uint64_t)hdr.e_phnum * hdr.e_phentsize;
    if (ph_end_check > file->size) return 0;

    uint32_t highest = 0;
    for (int i = 0; i < hdr.e_phnum; i++) {
        Elf32_Phdr ph;
        if (read_vfs(file,
                     hdr.e_phoff + (uint64_t)i * hdr.e_phentsize,
                     sizeof(Elf32_Phdr), (char*)&ph) <= 0)
            break;
        if (ph.p_type != PT_LOAD) continue;
        if (ph.p_vaddr + ph.p_memsz < ph.p_vaddr) continue;
        uint32_t end = ph.p_vaddr + ph.p_memsz;
        if (end > highest) highest = end;
    }
 
    return (highest + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
}

#include "task.h"
#include "klib.h"

void elf_load_exec_symtab(const char* path, struct proc_metadata* proc) {
    if (!path || !proc) return;
    proc->exec_symtab = 0;
    proc->exec_strtab = 0;
    proc->exec_symtab_count = 0;

    vfs_node_t* base = (path[0] == '/') ? vfs_root : vfs_root;
    struct vfs_node* file = vfs_walk_path(base, (char*)path);
    if (!file) return;

    Elf32_Ehdr hdr;
    if (read_vfs(file, 0, sizeof(Elf32_Ehdr), (char*)&hdr) <= 0) return;
    if (*(uint32_t*)hdr.e_ident != ELF_MAGIC) return;
    if (hdr.e_shentsize < sizeof(Elf32_Shdr)) return;
    if ((uint32_t)hdr.e_shnum > 256) return;

    // Find the PT_LOAD segment with the lowest vaddr to determine base
    uint32_t load_base = 0xFFFFFFFF;
    for (int i = 0; i < hdr.e_phnum && hdr.e_phentsize >= sizeof(Elf32_Phdr); i++) {
        Elf32_Phdr ph;
        if (read_vfs(file, hdr.e_phoff + (uint64_t)i * hdr.e_phentsize,
                     sizeof(Elf32_Phdr), (char*)&ph) <= 0) break;
        if (ph.p_type == PT_LOAD && ph.p_vaddr < load_base)
            load_base = ph.p_vaddr;
    }
    proc->exec_base = (load_base == 0xFFFFFFFF) ? 0 : load_base;

    // Read section headers
    Elf32_Shdr* shdrs = kmalloc(hdr.e_shnum * sizeof(Elf32_Shdr));
    if (!shdrs) return;
    if (read_vfs(file, hdr.e_shoff, hdr.e_shnum * sizeof(Elf32_Shdr),
                 (char*)shdrs) <= 0) {
        kfree(shdrs);
        return;
    }

    // Read section header string table
    char* shstrtab = 0;
    if (hdr.e_shstrndx < hdr.e_shnum && shdrs[hdr.e_shstrndx].sh_size > 0) {
        shstrtab = kmalloc(shdrs[hdr.e_shstrndx].sh_size);
        if (shstrtab) {
            read_vfs(file, shdrs[hdr.e_shstrndx].sh_offset,
                     shdrs[hdr.e_shstrndx].sh_size, shstrtab);
        }
    }

    // Find SHT_SYMTAB and its linked SHT_STRTAB
    Elf32_Shdr* sym_sh = 0;
    Elf32_Shdr* str_sh = 0;
    for (int i = 0; i < hdr.e_shnum; i++) {
        if (shdrs[i].sh_type == SHT_SYMTAB && !sym_sh) sym_sh = &shdrs[i];
        if (shdrs[i].sh_type == SHT_STRTAB && shdrs[i].sh_entsize == 0) str_sh = &shdrs[i];
    }

    // Use the linked strtab from the symtab section
    if (sym_sh && sym_sh->sh_link < hdr.e_shnum)
        str_sh = &shdrs[sym_sh->sh_link];

    if (sym_sh && str_sh && sym_sh->sh_size > 0) {
        proc->exec_symtab = kmalloc(sym_sh->sh_size);
        proc->exec_strtab = kmalloc(str_sh->sh_size);
        if (proc->exec_symtab && proc->exec_strtab) {
            read_vfs(file, sym_sh->sh_offset, sym_sh->sh_size, (char*)proc->exec_symtab);
            read_vfs(file, str_sh->sh_offset, str_sh->sh_size, proc->exec_strtab);
            proc->exec_symtab_count = sym_sh->sh_size / sizeof(Elf32_Sym);
        } else {
            if (proc->exec_symtab) { kfree(proc->exec_symtab); proc->exec_symtab = 0; }
            if (proc->exec_strtab) { kfree(proc->exec_strtab); proc->exec_strtab = 0; }
        }
    }

    if (shstrtab) kfree(shstrtab);
    kfree(shdrs);
}