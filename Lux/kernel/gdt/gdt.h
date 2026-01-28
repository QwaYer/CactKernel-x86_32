#ifndef GDT_H
#define GDT_H

struct gdt_entry {
    unsigned short limit_low;
    unsigned short base_low;
    unsigned char  base_middle;
    unsigned char  access;
    unsigned char  granularity;
    unsigned char  base_high;
} __attribute__((packed));

struct gdt_ptr {
    unsigned short limit;
    unsigned int   base;
} __attribute__((packed));

struct tss_entry_struct {
    unsigned int prev_tss;
    unsigned int esp0;    
    unsigned int ss0;    
    unsigned int esp1, ss1, esp2, ss2;
    unsigned int cr3;
    unsigned int eip, eflags, eax, ecx, edx, ebx, esp, ebp, esi, edi;
    unsigned int es, cs, ss, ds, fs, gs;
    unsigned int ldt;
    unsigned short trap, iomap_base;
} __attribute__((packed));

void init_gdt();
void set_gdt_gate(int num, unsigned int base, unsigned int limit, unsigned char access, unsigned char gran);
void write_tss(int num, unsigned short ss0, unsigned int esp0);

#endif