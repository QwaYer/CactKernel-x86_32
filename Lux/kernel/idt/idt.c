#include "idt.h"
#include "kernel.h"  
#include "memory.h"  

static struct idt_entry idt[256];
static struct idt_ptr   idtp;

void set_idt_gate(int n, unsigned int handler) {
    idt[n].low_offset  = (unsigned short)(handler & 0xFFFF);
    idt[n].sel         = 0x08;
    idt[n].always0     = 0;
    idt[n].flags       = 0x8E;
    idt[n].high_offset = (unsigned short)((handler >> 16) & 0xFFFF);
}

/* 8259 PIC:
   IRQ0-7  -> INT 0x20-0x27  (master)
   IRQ8-15 -> INT 0x28-0x2F  (slave)
   Маска: открыты IRQ0 (таймер) и IRQ1 (клавиатура). */
void init_pic() {
    /* ICW1: начало инициализации */
    port_byte_out(0x20, 0x11);
    port_byte_out(0xA0, 0x11);
    /* ICW2: базовые векторы */
    port_byte_out(0x21, 0x20);
    port_byte_out(0xA1, 0x28);
    /* ICW3: каскадирование */
    port_byte_out(0x21, 0x04);
    port_byte_out(0xA1, 0x02);
    /* ICW4: режим 8086 */
    port_byte_out(0x21, 0x01);
    port_byte_out(0xA1, 0x01);
    /* OCW1: маски — разрешается только IRQ0 и IRQ1 */
    port_byte_out(0x21, 0xFC);
    port_byte_out(0xA1, 0xFF);
}

int init_idt() {
    idtp.limit = (sizeof(struct idt_entry) * 256) - 1;
    idtp.base  = (unsigned int)&idt;
    memory_set(&idt, 0, sizeof(struct idt_entry) * 256);

    /* Исключения CPU */
    set_idt_gate(0,  (unsigned int)isr0);
    set_idt_gate(13, (unsigned int)isr13);
    set_idt_gate(14, (unsigned int)isr14);
    for (int i = 1; i < 32; i++)
        if (i != 13 && i != 14)
            set_idt_gate(i, (unsigned int)isr_common_stub);

    /* IRQ */
    set_idt_gate(32, (unsigned int)timer_isr);
    set_idt_gate(33, (unsigned int)keyboard_isr);

    /* Системный вызов (int 0x80) — DPL=3 чтобы вызывать из userspace */
    set_idt_gate(128, (unsigned int)syscall_isr);
    idt[128].flags = 0xEE;

    __asm__ __volatile__("lidt (%0)" : : "r"(&idtp));
    return 0;
}