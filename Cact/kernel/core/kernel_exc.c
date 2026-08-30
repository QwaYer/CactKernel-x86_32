#include "kernel.h"
#include "multiboot2.h"
#include "memory.h"
#include "gdt.h"
#include "idt.h"
#include "klib.h"
#include "task.h"
#include "fb.h"
#include "apic.h"
#include "msi.h"
#include "sym.h"

// CPU exception handler — signals for user tasks, panic for kernel
void dump_context_frame(struct context_frame* regs, uint32_t fault_addr, uint32_t signal) {
    char buf[32];
    const char* exc_names[32] = {
        "DE", "DB", "NMI", "BP", "OF", "BR", "UD", "NM",
        "DF", "CSO", "TS", "NP", "SS", "GP", "PF", "MF",
        "AC", "MC", "XF", "??", "??", "??", "??", "??",
        "??", "??", "??", "??", "??", "??", "??", "??"
    };

    printk_color("\n=== ", COLOR_LIGHT_RED);
    if (signal && current_task && !current_task->is_kernel) {
        printk_color("SIGNAL ", COLOR_LIGHT_RED);
        snprintf(buf, sizeof(buf), "0x%x", (unsigned)(signal)); printk_color(buf, COLOR_LIGHT_RED);
        printk_color(" (pid=", COLOR_LIGHT_RED);
        snprintf(buf, sizeof(buf), "%d", (int)((int)current_task->pid)); printk_color(buf, COLOR_LIGHT_RED);
        printk_color(")", COLOR_LIGHT_RED);
    } else {
        printk_color("PANIC", COLOR_LIGHT_RED);
    }
    printk_color(" ===\n", COLOR_LIGHT_RED);

    printk_color("Exception: ", COLOR_LIGHT_RED);
    snprintf(buf, sizeof(buf), "%d", (int)((int)regs->int_no)); printk(buf);
    printk(" ("); printk((char*)exc_names[regs->int_no < 32 ? regs->int_no : 31]);
    printk(")\n");

    if (regs->int_no == 14) {
        printk_color("Fault address: 0x", COLOR_LIGHT_RED);
        snprintf(buf, sizeof(buf), "0x%x", (unsigned)(fault_addr)); printk_color(buf, COLOR_LIGHT_RED);

        uint32_t err = regs->err_code;
        printk_color("  Error code: 0x", COLOR_LIGHT_RED);
        snprintf(buf, sizeof(buf), "0x%x", (unsigned)(err)); printk_color(buf, COLOR_LIGHT_RED);
        printk_color(" [", COLOR_LIGHT_RED);
        if (err & 1) printk_color("PROT", COLOR_LIGHT_RED);
        else         printk_color("NP ", COLOR_LIGHT_RED);
        if (err & 2) printk_color(" W", COLOR_LIGHT_RED);
        else         printk_color(" R", COLOR_LIGHT_RED);
        if (err & 4) printk_color(" U", COLOR_LIGHT_RED);
        else         printk_color(" S", COLOR_LIGHT_RED);
        if (err & 8) printk(" RSVD");
        if (err & 16) printk(" IF");
        printk_color(" ]\n", COLOR_LIGHT_RED);
    } else {
        printk_color("Error code: 0x", COLOR_LIGHT_RED);
        snprintf(buf, sizeof(buf), "0x%x", (unsigned)(regs->err_code)); printk_color(buf, COLOR_LIGHT_RED);
        printk("\n");
    }

    printk_color(" EIP: 0x", COLOR_LIGHT_RED);
    snprintf(buf, sizeof(buf), "0x%x", (unsigned)(regs->eip)); printk_color(buf, COLOR_LIGHT_RED);
    printk_color("  CS: 0x", COLOR_LIGHT_RED);
    snprintf(buf, sizeof(buf), "0x%x", (unsigned)(regs->cs)); printk_color(buf, COLOR_LIGHT_RED);
    printk_color(" EFLAGS: 0x", COLOR_LIGHT_RED);
    snprintf(buf, sizeof(buf), "0x%x", (unsigned)(regs->eflags)); printk_color(buf, COLOR_LIGHT_RED);
    printk("\n");

    printk_color("EAX: 0x", COLOR_LIGHT_GREEN);
    snprintf(buf, sizeof(buf), "0x%x", (unsigned)(regs->eax)); printk_color(buf, COLOR_LIGHT_GREEN);
    printk_color(" EBX: 0x", COLOR_LIGHT_GREEN);
    snprintf(buf, sizeof(buf), "0x%x", (unsigned)(regs->ebx)); printk_color(buf, COLOR_LIGHT_GREEN);
    printk_color(" ECX: 0x", COLOR_LIGHT_GREEN);
    snprintf(buf, sizeof(buf), "0x%x", (unsigned)(regs->ecx)); printk_color(buf, COLOR_LIGHT_GREEN);
    printk_color(" EDX: 0x", COLOR_LIGHT_GREEN);
    snprintf(buf, sizeof(buf), "0x%x", (unsigned)(regs->edx)); printk_color(buf, COLOR_LIGHT_GREEN);
    printk("\n");

    printk_color("ESI: 0x", COLOR_LIGHT_GREEN);
    snprintf(buf, sizeof(buf), "0x%x", (unsigned)(regs->esi)); printk_color(buf, COLOR_LIGHT_GREEN);
    printk_color(" EDI: 0x", COLOR_LIGHT_GREEN);
    snprintf(buf, sizeof(buf), "0x%x", (unsigned)(regs->edi)); printk_color(buf, COLOR_LIGHT_GREEN);
    printk_color(" EBP: 0x", COLOR_LIGHT_GREEN);
    snprintf(buf, sizeof(buf), "0x%x", (unsigned)(regs->ebp)); printk_color(buf, COLOR_LIGHT_GREEN);
    printk_color(" ESP: 0x", COLOR_LIGHT_GREEN);
    snprintf(buf, sizeof(buf), "0x%x", (unsigned)(regs->useresp)); printk_color(buf, COLOR_LIGHT_GREEN);
    printk("\n");

    printk_color(" DS: 0x", COLOR_LIGHT_GREEN);
    snprintf(buf, sizeof(buf), "0x%x", (unsigned)(regs->ds)); printk_color(buf, COLOR_LIGHT_GREEN);
    printk_color(" ES: 0x", COLOR_LIGHT_GREEN);
    snprintf(buf, sizeof(buf), "0x%x", (unsigned)(regs->es)); printk_color(buf, COLOR_LIGHT_GREEN);
    printk_color(" SS: 0x", COLOR_LIGHT_GREEN);
    snprintf(buf, sizeof(buf), "0x%x", (unsigned)(regs->ss)); printk_color(buf, COLOR_LIGHT_GREEN);
    printk("\n");

    // Stack trace — walk EBP chain
    printk_color("Call trace:\n", COLOR_LIGHT_BROWN);
    int frames = 0;
    uint32_t ebp = regs->ebp;
    int is_kernel_fault = (regs->cs == 0x08);

    if (is_kernel_fault) {
        while (ebp >= 0xC0000000 && ebp < 0xC0100000 && frames < 16) {
            uint32_t ret_eip = ((uint32_t*)ebp)[1];
            uint32_t next_ebp = ((uint32_t*)ebp)[0];
            printk_color("  [", COLOR_LIGHT_BROWN);
            snprintf(buf, sizeof(buf), "%d", (int)(frames)); printk_color(buf, COLOR_LIGHT_BROWN);
            printk_color("] 0x", COLOR_LIGHT_BROWN);
            snprintf(buf, sizeof(buf), "0x%x", (unsigned)(ret_eip)); printk_color(buf, COLOR_LIGHT_BROWN);
            {
                uint32_t sym_off;
                const char* sym = sym_resolve_addr(ret_eip, &sym_off);
                if (sym) {
                    printk_color(" (", COLOR_DARK_GREY);
                    printk((char*)sym);
                    printk_color("+", COLOR_DARK_GREY);
                    snprintf(buf, sizeof(buf), "0x%x", (unsigned)(sym_off)); printk_color(buf, COLOR_DARK_GREY);
                    printk_color(")", COLOR_DARK_GREY);
                }
            }
            printk_color("\n", COLOR_LIGHT_BROWN);
            if (next_ebp <= ebp) break;
            ebp = next_ebp;
            frames++;
        }
    }
    if (frames == 0) {
        printk_color("  (no trace)\n", COLOR_DARK_GREY);
    }

    // Print EIP instruction bytes
    printk_color("Code: ", COLOR_LIGHT_BROWN);
    if (is_kernel_fault) {
        uint32_t* pd = is_kernel_fault ? get_current_pd() : current_task->page_directory;
        // Read instruction bytes at EIP
        for (int i = -4; i < 8; i++) {
            uint32_t addr = regs->eip + i;
            if (addr < 0x1000 || addr >= 0xC0000000) { printk("?? "); continue; }
            uint32_t pdi = PD_INDEX(addr);
            uint32_t pti = PT_INDEX(addr);
            if (!(pd[pdi] & PAGE_PRESENT)) { printk("?? "); continue; }
            uint32_t* pt = (uint32_t*)(pd[pdi] & ~0xFFFu);
            if (!(pt[pti] & PAGE_PRESENT)) { printk("?? "); continue; }
            uint32_t phys = (pt[pti] & ~0xFFFu) + (addr & 0xFFFu);
            uint8_t byte = *(volatile uint8_t*)(uintptr_t)phys;
            snprintf(buf, sizeof(buf), "0x%x", (unsigned)byte);
            if (i == 0) printk_color("<", COLOR_LIGHT_GREEN);
            printk(buf);
            if (i == 0) printk_color(">", COLOR_LIGHT_GREEN);
            printk(" ");
        }
    }
    printk("\n");
}

// CPU exception handler — signals for user tasks, panic for kernel
void exception_handler(struct context_frame* regs) {
    uint32_t signal = 0;
    if (current_task && !current_task->is_kernel) {
        switch (regs->int_no) {
        case 0: case 16: signal = SIGFPE; break;
        case 13: case 14: signal = SIGSEGV; break;
        default: signal = SIGKILL; break;
        }
    }
    dump_context_frame(regs, read_cr2(), signal);

    if (signal && current_task && !current_task->is_kernel) {
        task_signal(current_task->pid, signal);
        schedule();
        return;
    }

    printk_color("System halted.", COLOR_LIGHT_RED);
    while(1);
}

void timer_eoi(void) {
    apic_eoi();
}

void irq_apic_eoi(void) {
    apic_eoi();
}
