#include "gdt.h"
#include "libc.h"

struct gdt_entry gdt[6];
struct gdt_ptr   gp;
struct tss_entry_struct tss_entry;

extern void gdt_flush(unsigned int);
extern void tss_flush();

void set_gdt_gate(int num, unsigned int base, unsigned int limit, unsigned char access, unsigned char gran) {
    gdt[num].base_low    = (base & 0xFFFF);
    gdt[num].base_middle = (base >> 16) & 0xFF;
    gdt[num].base_high   = (base >> 24) & 0xFF;
    gdt[num].limit_low   = (limit & 0xFFFF);
    gdt[num].granularity = (limit >> 16) & 0x0F;
    gdt[num].granularity |= gran & 0xF0;
    gdt[num].access      = access;
}

void write_tss(int num, unsigned short ss0, unsigned int esp0) {
    unsigned int base = (unsigned int)&tss_entry;
    unsigned int limit = sizeof(tss_entry) - 1;

    set_gdt_gate(num, base, limit, 0xE9, 0x00);
    memory_set(&tss_entry, 0, sizeof(tss_entry));

    tss_entry.ss0 = ss0;
    tss_entry.esp0 = esp0;
    tss_entry.iomap_base = sizeof(tss_entry);
}

void init_gdt() {
    gp.limit = (sizeof(struct gdt_entry) * 6) - 1;
    gp.base  = (unsigned int)&gdt;

    set_gdt_gate(0, 0, 0, 0, 0);                // Null segment (0x00)
    set_gdt_gate(1, 0, 0xFFFFFFFF, 0x9A, 0xCF); // Kernel Code (0x08)
    set_gdt_gate(2, 0, 0xFFFFFFFF, 0x92, 0xCF); // Kernel Data (0x10)
    set_gdt_gate(3, 0, 0xFFFFFFFF, 0xFA, 0xCF); // User Code (0x1B)
    set_gdt_gate(4, 0, 0xFFFFFFFF, 0xF2, 0xCF); // User Data (0x23)
    write_tss(5, 0x10, 0);                      // TSS (0x28)

    gdt_flush((unsigned int)&gp);
    tss_flush();
}