#ifndef IDT_H
#define IDT_H

#include <stdint.h>

struct idt_entry {
    uint16_t low_offset;
    uint16_t sel;
    uint8_t  always0;
    uint8_t  flags;
    uint16_t high_offset;
} __attribute__((packed));

struct idt_ptr {
    uint16_t limit;
    uint32_t base;
} __attribute__((packed));

void set_idt_gate(int n, uint32_t handler);
void init_pic();
int  init_idt();

#endif