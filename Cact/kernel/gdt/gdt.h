#ifndef GDT_H
#define GDT_H

#include <stdint.h>

// GDT entry (8 bytes, packed)
struct gdt_entry {
    uint16_t limit_low;     // Bits 0-15 of segment limit
    uint16_t base_low;      // Bits 0-15 of base address
    uint8_t  base_middle;   // Bits 16-23 of base address
    uint8_t  access;        // Access flags (P, DPL, Type)
    uint8_t  granularity;   // Limit high (4 bits) + G, DB, L flags
    uint8_t  base_high;     // Bits 24-31 of base address
} __attribute__((packed));

// GDTR structure (48-bit pseudo-descriptor)
struct gdt_ptr {
    uint16_t limit;         // Size of GDT - 1
    uint32_t base;          // Linear address of GDT
} __attribute__((packed));

// TSS (Task State Segment) for hardware task switching / ring0 stack
struct tss_entry_struct {
    uint32_t prev_tss;      // Previous TSS (for hardware task switching)
    uint32_t esp0;          // Stack pointer for ring0
    uint32_t ss0;           // Stack segment for ring0
    uint32_t esp1, ss1;     // Ring1 stack (unused)
    uint32_t esp2, ss2;     // Ring2 stack (unused)
    uint32_t cr3;           // Page directory base (unused)
    uint32_t eip, eflags, eax, ecx, edx, ebx, esp, ebp, esi, edi;  // Saved registers
    uint32_t es, cs, ss, ds, fs, gs;  // Saved segment selectors
    uint32_t ldt;           // LDT selector (unused)
    uint16_t trap;          // Debug trap flag
    uint16_t iomap_base;    // I/O permission bitmap base offset
} __attribute__((packed));

// GDT initialization
void init_gdt();
void set_gdt_gate(int num, uint32_t base, uint32_t limit, uint8_t access, uint8_t gran);
void write_tss(int num, uint16_t ss0, uint32_t esp0);

// External assembly functions
extern struct tss_entry_struct tss_entry;
extern void gdt_flush(uint32_t);  // Load GDTR and far jump
extern void tss_flush(void);      // Load Task Register (LTR)

#endif 