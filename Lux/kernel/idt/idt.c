#include "idt.h"
#include "kernel.h"
#include "memory.h"
#include "libc.h"

extern void isr0();
extern void isr13();
extern void isr14();
extern void isr_common_stub();
extern void timer_isr();
extern void keyboard_isr();
extern void virtio_net_isr();
extern void syscall_isr();

static struct idt_entry idt[256];
static struct idt_ptr   idtp;

void set_idt_gate(int n, uint32_t handler) {
    idt[n].low_offset  = (uint16_t)(handler & 0xFFFF);
    idt[n].sel         = 0x08;
    idt[n].always0     = 0;
    idt[n].flags       = 0x8E;
    idt[n].high_offset = (uint16_t)((handler >> 16) & 0xFFFF);
}

/* 8259 PIC: */
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
    /* OCW1: маски — разрешается IRQ0, IRQ1 и IRQ11 (VirtIO) */
    port_byte_out(0x21, 0xFC);
    port_byte_out(0xA1, 0xF7);
}

int init_idt() {
    idtp.limit = (sizeof(struct idt_entry) * 256) - 1;
    idtp.base  = (uint32_t)&idt;
    memory_set(&idt, 0, sizeof(struct idt_entry) * 256);

    /* Исключения CPU */
    set_idt_gate(0,  (uint32_t)isr0);
    set_idt_gate(13, (uint32_t)isr13);
    set_idt_gate(14, (uint32_t)isr14);
    for (int i = 1; i < 32; i++)
        if (i != 13 && i != 14)
            set_idt_gate(i, (uint32_t)isr_common_stub);

    /* IRQ */
    set_idt_gate(32, (uint32_t)timer_isr);
    set_idt_gate(33, (uint32_t)keyboard_isr);
    set_idt_gate(0x2B, (uint32_t)virtio_net_isr);

    /* Системный вызов (int 0x80) — DPL=3 чтобы вызывать из userspace */
    set_idt_gate(128, (uint32_t)syscall_isr);
    idt[128].flags = 0xEE;

    __asm__ __volatile__("lidt (%0)" : : "r"(&idtp));
    return 0;
}