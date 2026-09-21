#include "kernel.h"
#include "klog.h"
#include "multiboot2.h"
#include "memory.h"
#include "gdt.h"
#include "idt.h"
#include "klib.h"
#include "task.h"
#include "validate.h"
#include "proc.h"
#include "fb.h"
#include "apic.h"
#include "msi.h"
#include "sym.h"

#define TRACE_FRAMES_MAX 16

// printk_color_level() emits its string verbatim, so the "0x" prefix has to be
// part of the formatted value exactly once.
static void put_hex(uint32_t v, uint32_t color) {
    char buf[16];
    snprintf(buf, sizeof(buf), "0x%x", (unsigned)v);
    printk_color_level(KLOG_LEVEL_CRIT, buf, color);
}

static void put_label_hex(char* label, uint32_t v, uint32_t color) {
    printk_color_level(KLOG_LEVEL_CRIT, label, color);
    put_hex(v, color);
}

// User space is [USER_SPACE_START, KERNEL_BASE); everything else is kernel:
// low identity-mapped RAM, the kernel image, and the kalloc()'d kernel stacks.
static int in_kernel_space(uint32_t addr) {
    return addr < USER_SPACE_START || addr >= KERNEL_BASE;
}

// Read a u32 from a user address through the task's page tables.  The mapping
// is checked first, so a wild user EBP cannot re-fault the handler.
static int peek_user_u32(uint32_t* pd, uint32_t addr, uint32_t* out) {
    if (!pd) return -1;
    uint32_t pdi = PD_INDEX(addr);
    uint32_t pti = PT_INDEX(addr);
    if (!(pd[pdi] & PAGE_PRESENT)) return -1;
    uint32_t* pt = (uint32_t*)(pd[pdi] & ~0xFFFu);
    if (!(pt[pti] & PAGE_PRESENT)) return -1;
    *out = *(volatile uint32_t*)((pt[pti] & ~0xFFFu) + (addr & 0xFFFu));
    return 0;
}

static void print_trace_frame(int idx, uint32_t ret_eip) {
    char buf[16];
    printk_color_level(KLOG_LEVEL_CRIT, "  [", COLOR_LIGHT_BROWN);
    snprintf(buf, sizeof(buf), "%d", idx);
    printk_color_level(KLOG_LEVEL_CRIT, buf, COLOR_LIGHT_BROWN);
    printk_color_level(KLOG_LEVEL_CRIT, "] ", COLOR_LIGHT_BROWN);
    put_hex(ret_eip, COLOR_LIGHT_BROWN);

    uint32_t sym_off;
    const char* sym = sym_resolve_addr(ret_eip, &sym_off);
    if (sym) {
        printk_color_level(KLOG_LEVEL_CRIT, " (", COLOR_DARK_GREY);
        printk((char*)sym);
        printk_color_level(KLOG_LEVEL_CRIT, "+", COLOR_DARK_GREY);
        snprintf(buf, sizeof(buf), "0x%x", (unsigned)sym_off);
        printk_color_level(KLOG_LEVEL_CRIT, buf, COLOR_DARK_GREY);
        printk_color_level(KLOG_LEVEL_CRIT, ")", COLOR_DARK_GREY);
    }
    printk_color_level(KLOG_LEVEL_CRIT, "\n", COLOR_LIGHT_BROWN);
}

// CPU exception handler — signals for user tasks, panic for kernel
void dump_context_frame(struct context_frame* regs, uint32_t fault_addr, uint32_t signal) {
    char buf[32];
    const char* exc_names[32] = {
        "DE", "DB", "NMI", "BP", "OF", "BR", "UD", "NM",
        "DF", "CSO", "TS", "NP", "SS", "GP", "PF", "MF",
        "AC", "MC", "XF", "??", "??", "??", "??", "??",
        "??", "??", "??", "??", "??", "??", "??", "??"
    };

    int is_user_task = (current_task && !current_task->is_kernel);
    // Only a ring3 -> ring0 delivery makes the CPU push SS:ESP, so the frame is
    // 8 bytes larger and regs->useresp/ss are meaningful only in that case.
    int priv_change = (regs->cs & 3u) != 0;
    // The stub pushed the whole frame *below* the faulting ESP, so the faulting
    // ESP itself is regs + sizeof(context_frame), minus the 8 bytes the CPU
    // only pushes across a privilege change.
    uint32_t fault_esp = (uint32_t)regs + (priv_change ? 68u : 60u);

    printk_color_level(KLOG_LEVEL_CRIT, "\n=== ", COLOR_LIGHT_RED);
    if (signal && is_user_task) {
        printk_color_level(KLOG_LEVEL_CRIT, "SIGNAL ", COLOR_LIGHT_RED);
        put_hex(signal, COLOR_LIGHT_RED);
        printk_color_level(KLOG_LEVEL_CRIT, " (pid=", COLOR_LIGHT_RED);
        snprintf(buf, sizeof(buf), "%d", (int)((int)current_task->pid)); printk_color_level(KLOG_LEVEL_CRIT, buf, COLOR_LIGHT_RED);
        printk_color_level(KLOG_LEVEL_CRIT, ")", COLOR_LIGHT_RED);
    } else {
        printk_color_level(KLOG_LEVEL_CRIT, "PANIC", COLOR_LIGHT_RED);
    }
    printk_color_level(KLOG_LEVEL_CRIT, " ===\n", COLOR_LIGHT_RED);

    printk_color_level(KLOG_LEVEL_CRIT, "Exception: ", COLOR_LIGHT_RED);
    snprintf(buf, sizeof(buf), "%d", (int)((int)regs->int_no)); printk(buf);
    printk(" ("); printk((char*)exc_names[regs->int_no < 32 ? regs->int_no : 31]);
    printk(")\n");

    if (regs->int_no == 14) {
        put_label_hex("Fault address: ", fault_addr, COLOR_LIGHT_RED);

        uint32_t err = regs->err_code;
        put_label_hex("  Error code: ", err, COLOR_LIGHT_RED);
        printk_color_level(KLOG_LEVEL_CRIT, " [", COLOR_LIGHT_RED);
        if (err & 1) printk_color_level(KLOG_LEVEL_CRIT, "PROT", COLOR_LIGHT_RED);
        else         printk_color_level(KLOG_LEVEL_CRIT, "NP ", COLOR_LIGHT_RED);
        if (err & 2) printk_color_level(KLOG_LEVEL_CRIT, " W", COLOR_LIGHT_RED);
        else         printk_color_level(KLOG_LEVEL_CRIT, " R", COLOR_LIGHT_RED);
        if (err & 4) printk_color_level(KLOG_LEVEL_CRIT, " U", COLOR_LIGHT_RED);
        else         printk_color_level(KLOG_LEVEL_CRIT, " S", COLOR_LIGHT_RED);
        if (err & 8) printk(" RSVD");
        if (err & 16) printk(" IF");
        printk_color_level(KLOG_LEVEL_CRIT, " ]\n", COLOR_LIGHT_RED);
    } else {
        put_label_hex("Error code: ", regs->err_code, COLOR_LIGHT_RED);
        printk("\n");
    }

    put_label_hex(" EIP: ", regs->eip, COLOR_LIGHT_RED);
    put_label_hex("  CS: ", regs->cs, COLOR_LIGHT_RED);
    put_label_hex(" EFLAGS: ", regs->eflags, COLOR_LIGHT_RED);
    printk("\n");

    put_label_hex("EAX: ", regs->eax, COLOR_LIGHT_GREEN);
    put_label_hex(" EBX: ", regs->ebx, COLOR_LIGHT_GREEN);
    put_label_hex(" ECX: ", regs->ecx, COLOR_LIGHT_GREEN);
    put_label_hex(" EDX: ", regs->edx, COLOR_LIGHT_GREEN);
    printk("\n");

    put_label_hex("ESI: ", regs->esi, COLOR_LIGHT_GREEN);
    put_label_hex(" EDI: ", regs->edi, COLOR_LIGHT_GREEN);
    put_label_hex(" EBP: ", regs->ebp, COLOR_LIGHT_GREEN);
    put_label_hex(" ESP: ", fault_esp, COLOR_LIGHT_GREEN);
    printk("\n");

    // Segment selectors: only the low 16 bits are the selector, the upper half
    // of a `push ds` (and of some saved frames) is architecturally undefined.
    put_label_hex(" DS: ", regs->ds & 0xFFFFu, COLOR_LIGHT_GREEN);
    put_label_hex(" ES: ", regs->es & 0xFFFFu, COLOR_LIGHT_GREEN);
    if (priv_change) {
        put_label_hex(" SS: ", regs->ss & 0xFFFFu, COLOR_LIGHT_GREEN);
        put_label_hex(" UESP: ", regs->useresp, COLOR_LIGHT_GREEN);
    } else {
        printk_color_level(KLOG_LEVEL_CRIT, " SS: <kernel>", COLOR_LIGHT_GREEN);
    }
    printk("\n");

    // Stack trace — walk the EBP chain.  A ring-0 fault runs on a kernel stack
    // (kalloc page, low memory), a ring-3 fault on the user stack; the two need
    // different address tests and the user one must go through the page tables.
    printk_color_level(KLOG_LEVEL_CRIT, "Call trace:\n", COLOR_LIGHT_BROWN);
    int frames = 0;
    uint32_t ebp = regs->ebp;

    if (in_kernel_space(ebp) && ebp >= 0x1000u) {
        while (frames < TRACE_FRAMES_MAX && (ebp & 3u) == 0) {
            uint32_t ret_eip  = ((uint32_t*)ebp)[1];
            uint32_t next_ebp = ((uint32_t*)ebp)[0];
            if (!in_kernel_space(ret_eip)) break;
            print_trace_frame(frames, ret_eip);
            frames++;
            if (next_ebp <= ebp) break;
            ebp = next_ebp;
        }
    } else {
        uint32_t* pd = current_task ? current_task->page_directory : 0;
        while (frames < TRACE_FRAMES_MAX && (ebp & 3u) == 0) {
            uint32_t ret_eip, next_ebp;
            if (peek_user_u32(pd, ebp,     &next_ebp) != 0) break;
            if (peek_user_u32(pd, ebp + 4, &ret_eip)  != 0) break;
            print_trace_frame(frames, ret_eip);
            frames++;
            if (next_ebp <= ebp) break;
            ebp = next_ebp;
        }
    }

    if (frames == 0) {
        printk_color_level(KLOG_LEVEL_CRIT, "  (no trace)\n", COLOR_DARK_GREY);
        // EBP was unusable — show the raw words at the faulting stack pointer,
        // which is where a corrupted return address / pointer ends up.
        printk_color_level(KLOG_LEVEL_CRIT, "Stack: ", COLOR_DARK_GREY);
        if (priv_change) {
            uint32_t* pd = current_task ? current_task->page_directory : 0;
            for (int i = 0; i < 12; i++) {
                uint32_t v;
                if (peek_user_u32(pd, regs->useresp + (uint32_t)i * 4u, &v) != 0) {
                    printk(" ??");
                } else {
                    printk(" "); put_hex(v, COLOR_DARK_GREY);
                }
            }
        } else {
            uint32_t* sp = (uint32_t*)fault_esp;
            for (int i = 0; i < 12; i++) { printk(" "); put_hex(sp[i], COLOR_DARK_GREY); }
        }
        printk_color_level(KLOG_LEVEL_CRIT, "\n", COLOR_DARK_GREY);
    }

    // Print EIP instruction bytes
    printk_color_level(KLOG_LEVEL_CRIT, "Code: ", COLOR_LIGHT_BROWN);
    {
        uint32_t* pd = in_kernel_space(regs->eip) ? get_current_pd()
                      : (current_task ? current_task->page_directory : 0);
        if (!pd) { printk("??\n"); return; }
        for (int i = -4; i < 8; i++) {
            uint32_t addr = regs->eip + (uint32_t)i;
            if (addr < 0x1000 || addr >= KERNEL_BASE) { printk("?? "); continue; }
            uint32_t pdi = PD_INDEX(addr);
            uint32_t pti = PT_INDEX(addr);
            if (!(pd[pdi] & PAGE_PRESENT)) { printk("?? "); continue; }
            uint32_t* pt = (uint32_t*)(pd[pdi] & ~0xFFFu);
            if (!(pt[pti] & PAGE_PRESENT)) { printk("?? "); continue; }
            uint32_t phys = (pt[pti] & ~0xFFFu) + (addr & 0xFFFu);
            uint8_t byte = *(volatile uint8_t*)(uintptr_t)phys;
            snprintf(buf, sizeof(buf), "0x%x", (unsigned)byte);
            if (i == 0) printk_color_level(KLOG_LEVEL_CRIT, "<", COLOR_LIGHT_GREEN);
            printk(buf);
            if (i == 0) printk_color_level(KLOG_LEVEL_CRIT, ">", COLOR_LIGHT_GREEN);
            printk(" ");
        }
    }
    printk("\n");
}

// CPU exception handler — signals for user tasks, panic for kernel
void exception_handler(struct context_frame* regs) {
    int is_user_task = (current_task && !current_task->is_kernel);
    uint32_t signal = 0;

    if (is_user_task) {
        switch (regs->int_no) {
        case 0: case 16: signal = SIGFPE; break;
        case 13: case 14: signal = SIGSEGV; break;
        default: signal = SIGKILL; break;
        }
    }

    dump_context_frame(regs, read_cr2(), signal);

    if (is_user_task) {
        // A user task that faults while CS is the *kernel* selector has a
        // corrupted ring-0 context (a bad call/jump/return target).  Returning
        // through iretd would re-execute the same address forever — the frame
        // is unchanged — so terminate the task outright instead.
        if (regs->cs == 0x08) {
            sched_task_exit(128 + 9);   // as if killed by SIGKILL
            for (;;)
                __asm__ volatile ("hlt");   // safety net if schedule() returns
        }

        task_signal(current_task->pid, signal);
        schedule();
        return;
    }

    printk_color_level(KLOG_LEVEL_CRIT, "System halted.", COLOR_LIGHT_RED);
    while(1);
}

void irq_apic_eoi(void) {
    apic_eoi();
}
