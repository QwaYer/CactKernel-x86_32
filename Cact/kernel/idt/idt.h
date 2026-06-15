#ifndef IDT_H
#define IDT_H

#include <stdint.h>

// IDT entry (8 bytes)
struct idt_entry {
    uint16_t low_offset;   // Lower 16 bits of handler address
    uint16_t sel;          // Code segment selector (kernel CS = 0x08)
    uint8_t  always0;      // Reserved, must be 0
    uint8_t  flags;        // Gate type, DPL, present flag
    uint16_t high_offset;  // Upper 16 bits of handler address
} __attribute__((packed));

// IDTR structure (48-bit pseudo-descriptor)
struct idt_ptr {
    uint16_t limit;        // Size of IDT - 1
    uint32_t base;         // Linear address of IDT
} __attribute__((packed));

void set_idt_gate(int n, uint32_t handler);
int  init_idt(void);

#endif  