#ifndef KERNEL_H
#define KERNEL_H

#include <stdint.h>
#include <stdbool.h>
#include "pci.h"
#include "irq.h"
#include "multiboot2.h"

// Forward declaration for exception frame
struct context_frame;

/*
 * Parsed multiboot2 information.
 *
 * Populated once in init() by walking bootloader-provided tag list.
 * Kernel consumes this flat view instead of raw tag memory.
 *
 * flags bit semantics (multiboot1-compatible):
 *   bit 0  = mem_lower / mem_upper valid
 *   bit 6  = mmap valid (mem_total_bytes populated)
 *   bit 12 = framebuffer info valid
 */
typedef struct {
    uint32_t flags;
    uint32_t mem_lower;          // Conventional memory (KB)
    uint32_t mem_upper;          // Extended memory (KB, capped at ~4GB-1MB)
    uint64_t mem_total_bytes;    // Sum of MMAP available regions

    uint64_t framebuffer_addr;
    uint32_t framebuffer_pitch;
    uint32_t framebuffer_width;
    uint32_t framebuffer_height;
    uint8_t  framebuffer_bpp;
    uint8_t  framebuffer_type;
} multiboot_info_t;

// Parse raw multiboot2 info into multiboot_info_t and MMAP table
void multiboot2_parse(uint32_t mb2_info_addr,
                      multiboot_info_t* out,
                      mb2_mmap_table_t* mmap_out);

// Framebuffer color definitions (RGB 24-bit)
typedef enum {
    COLOR_BLACK        = 0x000000,
    COLOR_BLUE         = 0x0000AA,
    COLOR_GREEN        = 0x00AA00,
    COLOR_CYAN         = 0x00AAAA,
    COLOR_RED          = 0xAA0000,
    COLOR_MAGENTA      = 0xAA00AA,
    COLOR_BROWN        = 0xAA5500,
    COLOR_LIGHT_GREY   = 0xAAAAAA,
    COLOR_DARK_GREY    = 0x555555,
    COLOR_LIGHT_BLUE   = 0x5555FF,
    COLOR_LIGHT_GREEN  = 0x55FF55,
    COLOR_LIGHT_CYAN   = 0x55FFFF,
    COLOR_LIGHT_RED    = 0xFF5555,
    COLOR_LIGHT_MAGENTA= 0xFF55FF,
    COLOR_LIGHT_BROWN  = 0xFFFF55,
    COLOR_WHITE        = 0xFFFFFF
} fb_color_t;

#define WHITE_ON_BLACK COLOR_WHITE

// Page table entry flags
#define PAGE_PRESENT 0x1
#define PAGE_RW      0x2
#define PAGE_USER    0x4

// Page directory / table index extraction
#define PD_INDEX(vaddr) ((vaddr >> 22) & 0x3FF)
#define PT_INDEX(vaddr) ((vaddr >> 12) & 0x3FF)

// I/O port operations (inline assembly)
extern void     port_byte_out(uint16_t port, uint8_t data);
extern uint8_t  port_byte_in (uint16_t port);
extern void     port_word_out(uint16_t port, uint16_t data);
extern uint16_t port_word_in (uint16_t port);
extern void     port_long_out(uint16_t port, uint32_t data);
extern uint32_t port_long_in (uint16_t port);

#define port_dword_out port_long_out
#define port_dword_in  port_long_in

// Interrupt Service Routine stubs (defined in isr.S)
extern void timer_isr();
extern void keyboard_isr();
extern void syscall_isr();
extern void isr0();  extern void isr1();  extern void isr2();
extern void isr3();  extern void isr4();  extern void isr5();
extern void isr6();  extern void isr7();  extern void isr8();
extern void isr9();  extern void isr10(); extern void isr11();
extern void isr12(); extern void isr13(); extern void isr14();
extern void isr15(); extern void isr16(); extern void isr17();
extern void isr18(); extern void isr19(); extern void isr20();
extern void isr21(); extern void isr22(); extern void isr23();
extern void isr24(); extern void isr25(); extern void isr26();
extern void isr27(); extern void isr28(); extern void isr29();
extern void isr30(); extern void isr31();
extern void isr_common_stub();

// Log levels for klog()
typedef enum {
    LOG_OK    = 0,
    LOG_WARN  = 1,
    LOG_ERROR = 2,
    LOG_FAIL  = 3,
} log_level_t;

// Framebuffer console I/O
void kprint      (char* message);
void kprint_color(char* message, uint32_t color);
void kprint_at   (char* message, int x, int y);
void clear_screen(void);
void scroll      (void);
void klog        (log_level_t level, const char* message);
int  get_cursor_x(void);
int  get_cursor_y(void);

// Kernel-space string/number utilities
void itoa          (int n, char str[]);
int  atoi          (char* str);
int  strcmp        (const char* s1, const char* s2);
int  compare_string(const char* s1, const char* s2);
void hex_to_ascii  (uint32_t n, char str[]);

// CPU exception dispatcher
void exception_handler(struct context_frame* regs);

// Read CR2 (page fault linear address)
static inline uint32_t read_cr2(void) {
    uint32_t val;
    __asm__ __volatile__("mov %%cr2, %0" : "=r"(val));
    return val;
}

// Get current page directory pointer (CR3)
static inline uint32_t* get_current_pd(void) {
    uint32_t val;
    __asm__ __volatile__("mov %%cr3, %0" : "=r"(val));
    return (uint32_t*)val;
}

// Hardware detection helpers
int         probe_io_ports  (void);
int         detect_memory   (void);
void init_timer      (uint32_t frequency);

// Task scheduling
struct task_struct;
int                  init_scheduler(void);
void                 task_init     (void);
void                 schedule      (void);
void                 list_tasks    (void);
struct task_struct*  create_task   (void (*entry_point)(void));
extern void          switch_to     (uint32_t* old_esp, uint32_t new_esp);
extern struct task_struct* volatile current_task;

// Paging / memory management
void      init_paging            (void);
void      switch_paging          (uint32_t* pd);
void      vmm_map                (uint32_t* pd, uint32_t virt, uint32_t phys, int flags);
uint32_t  vmm_get_phys           (uint32_t* pd, uint32_t virt);
uint32_t* vmm_create_address_space(void);
uint32_t  get_free_heap_memory   (void);
void      slab_init              (void);
void      page_fault_init        (void);

// Generic block device layer
extern void blkdev_read_sector (uint32_t lba, uint8_t* buf);
extern void blkdev_write_sector(uint32_t lba, uint8_t* buf);

// Hardware setup entry
void kernel_setup_hardware(multiboot_info_t *mbi, mb2_mmap_table_t *mmap);

// ELF loader types
struct proc_page_tracker_t;
struct vfs_node;
void* load_elf(char* path, uint32_t* pd, struct proc_page_tracker_t* tracker);
uint32_t elf_get_brk_start(struct vfs_node* file);

// Networking stack
void net_init             (void);
void net_poll             (void);

// Terminal window size for TIOCGWINSZ/TIOCSWINSZ ioctls
#define TIOCGWINSZ 0x5413
#define TIOCSWINSZ 0x5414

struct winsize {
    uint16_t ws_row;
    uint16_t ws_col;
    uint16_t ws_xpixel;
    uint16_t ws_ypixel;
};

extern struct winsize terminal_winsize;
extern uint32_t       terminal_fg_pid;

// Global input device state
extern volatile int    keyboard_irq_count;
extern volatile uint8_t last_scancode_raw;
extern volatile char   last_char;
extern volatile int    key_event_happened;

#endif  