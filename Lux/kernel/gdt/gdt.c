#include "gdt.h"
#include "libc.h"
#include "kernel.h"

struct gdt_entry gdt[6];
struct gdt_ptr   gp;
struct tss_entry_struct tss_entry;

void set_gdt_gate(int num, uint32_t base, uint32_t limit, uint8_t access, uint8_t gran) {
    gdt[num].base_low    = (base & 0xFFFF);
    gdt[num].base_middle = (base >> 16) & 0xFF;
    gdt[num].base_high   = (base >> 24) & 0xFF;
    gdt[num].limit_low   = (limit & 0xFFFF);
    gdt[num].granularity = (limit >> 16) & 0x0F;
    gdt[num].granularity |= gran & 0xF0;
    gdt[num].access      = access;
}

void write_tss(int num, uint16_t ss0, uint32_t esp0) {
    uint32_t base = (uint32_t)&tss_entry;
    uint32_t limit = sizeof(tss_entry) - 1;

    set_gdt_gate(num, base, limit, 0xE9, 0x00);
    memory_set(&tss_entry, 0, sizeof(tss_entry));

    tss_entry.ss0 = ss0;
    tss_entry.esp0 = esp0;
    tss_entry.iomap_base = sizeof(tss_entry);
}

void init_gdt() {
    kprint("[GDT] base=0x");
    char buf[16]; hex_to_ascii((uint32_t)&gdt, buf); kprint(buf);
    kprint("  entries=6  limit=");
    itoa((int)(sizeof(struct gdt_entry) * 6 - 1), buf); kprint(buf); kprint("\n");

    gp.limit = (sizeof(struct gdt_entry) * 6) - 1;
    gp.base  = (uint32_t)&gdt;

    kprint("[GDT] 0x00 null  0x08 kernel-code  0x10 kernel-data"
           "  0x18 user-code  0x20 user-data\n");
    set_gdt_gate(0, 0, 0, 0, 0);                // Null segment (0x00)
    set_gdt_gate(1, 0, 0xFFFFFFFF, 0x9A, 0xCF); // Kernel Code (0x08)
    set_gdt_gate(2, 0, 0xFFFFFFFF, 0x92, 0xCF); // Kernel Data (0x10)
    set_gdt_gate(3, 0, 0xFFFFFFFF, 0xFA, 0xCF); // User Code (0x1B)
    set_gdt_gate(4, 0, 0xFFFFFFFF, 0xF2, 0xCF); // User Data (0x23)

    kprint("[GDT] 0x28 TSS  esp0=0x00000000  ss0=0x10\n");
    write_tss(5, 0x10, 0);                      // TSS (0x28)

    gdt_flush((uint32_t)&gp);
    kprint("[GDT] GDTR loaded  segment registers reloaded\n");
    tss_flush();
    kprint("[GDT] TR loaded  kernel ring0 CS=0x08 DS=0x10  user ring3 CS=0x1B DS=0x23\n");
    klog(LOG_OK, "GDT ready");
}