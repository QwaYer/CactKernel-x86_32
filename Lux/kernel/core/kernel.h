#ifndef KERNEL_H
#define KERNEL_H

#include <stdint.h>
#include <stdbool.h>
#include "pci.h"

// Forward declarations
struct context_frame;

// Multiboot Info Structure
typedef struct {
    uint32_t flags;
    uint32_t mem_lower;
    uint32_t mem_upper;
    uint32_t boot_device;
    uint32_t cmdline;
    uint32_t mods_count;
    uint32_t mods_addr;
    uint32_t num;
    uint32_t size;
    uint32_t addr;
    uint32_t shndx;
    uint32_t mmap_length;
    uint32_t mmap_addr;
    uint32_t drives_length;
    uint32_t drives_addr;
    uint32_t config_table;
    uint32_t boot_loader_name;
    uint32_t apm_table;
    uint32_t vbe_control_info;
    uint32_t vbe_mode_info;
    uint16_t vbe_mode;
    uint16_t vbe_interface_seg;
    uint16_t vbe_interface_off;
    uint16_t vbe_interface_len;
    uint64_t framebuffer_addr;
    uint32_t framebuffer_pitch;
    uint32_t framebuffer_width;
    uint32_t framebuffer_height;
    uint8_t  framebuffer_bpp;
    uint8_t  framebuffer_type;
    union {
        struct {
            uint32_t framebuffer_palette_addr;
            uint16_t framebuffer_palette_num_colors;
        };
        struct {
            uint8_t framebuffer_red_field_position;
            uint8_t framebuffer_red_mask_size;
            uint8_t framebuffer_green_field_position;
            uint8_t framebuffer_green_mask_size;
            uint8_t framebuffer_blue_field_position;
            uint8_t framebuffer_blue_mask_size;
        };
    };
} __attribute__((packed)) multiboot_info_t;

// Framebuffer Colors
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

// Paging Constants
#define PAGE_PRESENT 0x1
#define PAGE_RW      0x2
#define PAGE_USER    0x4

#define PD_INDEX(vaddr) ((vaddr >> 22) & 0x3FF)
#define PT_INDEX(vaddr) ((vaddr >> 12) & 0x3FF)

// I/O Port Operations
extern void     port_byte_out(uint16_t port, uint8_t data);
extern uint8_t  port_byte_in (uint16_t port);
extern void     port_word_out(uint16_t port, uint16_t data);
extern uint16_t port_word_in (uint16_t port);
extern void     port_long_out(uint16_t port, uint32_t data);
extern uint32_t port_long_in (uint16_t port);

#define port_dword_out port_long_out
#define port_dword_in  port_long_in

// Interrupt Service Routines
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

// Video / Console
void kprint      (char* message);
void kprint_color(char* message, uint32_t color);
void kprint_at   (char* message, int x, int y);
void clear_screen(void);
void scroll      (void);
void boot_log    (char* component, int status);
int  get_cursor_x(void);
int  get_cursor_y(void);

// Libc-like (Kernel Space)
void itoa          (int n, char str[]);
int  atoi          (char* str);
int  strcmp        (char* s1, char* s2);
int  compare_string(char* s1, char* s2);
void hex_to_ascii  (uint32_t n, char str[]);

// Exception Handling
void exception_handler(struct context_frame* regs);

static inline uint32_t read_cr2(void) {
    uint32_t val;
    __asm__ __volatile__("mov %%cr2, %0" : "=r"(val));
    return val;
}

static inline uint32_t* get_current_pd(void) {
    uint32_t val;
    __asm__ __volatile__("mov %%cr3, %0" : "=r"(val));
    return (uint32_t*)val;
}

// Hardware Initialization
int  init_framebuffer(void);
int  probe_io_ports  (void);
int  detect_memory   (void);
void init_timer      (uint32_t frequency);

// Task Management & Scheduling
struct task_struct;
int                  init_scheduler(void);
void                 task_init     (void);
void                 schedule      (void);
void                 list_tasks    (void);
struct task_struct*  create_task   (void (*entry_point)(void));
extern void          switch_to     (uint32_t* old_esp, uint32_t new_esp);
extern struct task_struct* volatile current_task;

// Memory Management
void      init_paging            (void);
void      switch_paging          (uint32_t* pd);
void      vmm_map                (uint32_t* pd, uint32_t virt, uint32_t phys, int flags);
uint32_t* vmm_create_address_space(void);
uint32_t  get_free_heap_memory   (void);
void      slab_init              (void);
void      page_fault_init        (void);

// Storage
extern void nvme_init        (void);
extern void nvme_read_sector (uint32_t lba, uint8_t* buf);
extern void nvme_write_sector(uint32_t lba, uint8_t* buf);

// Block Device Abstraction
extern void blkdev_read_sector (uint32_t lba, uint8_t* buf);
extern void blkdev_write_sector(uint32_t lba, uint8_t* buf);

// Kernel Entry Point Helpers
void kernel_setup_hardware(multiboot_info_t *mbi);

// ELF Loader
struct proc_page_tracker_t;
void* load_elf(char* path, uint32_t* pd, struct proc_page_tracker_t* tracker);

// Networking
void net_init             (void);
void net_poll             (void);
void virtio_net_irq_handler(void);

// Global State
extern volatile int    keyboard_irq_count;
extern volatile uint8_t last_scancode_raw;
extern volatile char   last_char;
extern volatile int    key_event_happened;

#endif