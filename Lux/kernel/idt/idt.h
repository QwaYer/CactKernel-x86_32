#ifndef IDT_H
#define IDT_H

struct idt_entry {
    unsigned short low_offset;
    unsigned short sel;
    unsigned char  always0;
    unsigned char  flags;
    unsigned short high_offset;
} __attribute__((packed));

struct idt_ptr {
    unsigned short limit;
    unsigned int   base;
} __attribute__((packed));

void set_idt_gate(int n, unsigned int handler);
void init_pic();
int  init_idt();

#endif