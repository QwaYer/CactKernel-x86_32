#ifndef KERNEL_H
#define KERNEL_H

#include <stdint.h>
#include <stdbool.h>

/* VGA Text Mode Constants */
#define VIDEO_ADDRESS 0xb8000
#define MAX_ROWS 25
#define MAX_COLS 80

/* VGA Colors */
typedef enum {
    COLOR_BLACK = 0,
    COLOR_BLUE = 1,
    COLOR_GREEN = 2,
    COLOR_CYAN = 3,
    COLOR_RED = 4,
    COLOR_MAGENTA = 5,
    COLOR_BROWN = 6,
    COLOR_LIGHT_GREY = 7,
    COLOR_DARK_GREY = 8,
    COLOR_LIGHT_BLUE = 9,
    COLOR_LIGHT_GREEN = 10,
    COLOR_LIGHT_CYAN = 11,
    COLOR_LIGHT_RED = 12,
    COLOR_LIGHT_MAGENTA = 13,
    COLOR_LIGHT_BROWN = 14,
    COLOR_WHITE = 15
} vga_color_t;

#define WHITE_ON_BLACK ((COLOR_BLACK << 4) | COLOR_WHITE)

/* Paging Constants */
#define PAGE_PRESENT 0x1
#define PAGE_RW      0x2
#define PAGE_USER    0x4

#define PD_INDEX(vaddr) ((vaddr >> 22) & 0x3FF)
#define PT_INDEX(vaddr) ((vaddr >> 12) & 0x3FF)

/* I/O Port Operations */
extern void port_byte_out(uint16_t port, uint8_t data);
extern uint8_t port_byte_in(uint16_t port);
extern void port_word_out(uint16_t port, uint16_t data);
extern uint16_t port_word_in(uint16_t port);
extern void port_long_out(uint16_t port, uint32_t data);
extern uint32_t port_long_in(uint16_t port);

/* Interrupt Service Routines */
extern void timer_isr();
extern void keyboard_isr();
extern void syscall_isr();
extern void isr0();
extern void isr13();
extern void isr14();
extern void isr_common_stub();

/* Video/Console Functions */
void kprint(char* message);
void kprint_color(char* message, uint8_t color);
void kprint_at(char* message, int x, int y);
void clear_screen();
void scroll();
void update_hardware_cursor(int x, int y);
void boot_log(char* component, int status);

/* Libc-like Functions (Kernel Space) */
void itoa(int n, char str[]);
int atoi(char* str);
int strcmp(char* s1, char* s2);
int compare_string(char* s1, char* s2);
void hex_to_ascii(uint32_t n, char str[]);

/* Exception Handling */
struct context_frame; 
void exception_handler(struct context_frame* regs);

static inline uint32_t read_cr2() {
    uint32_t val;
    __asm__ __volatile__("mov %%cr2, %0" : "=r"(val));
    return val;
}

static inline uint32_t* get_current_pd() {
    uint32_t val;
    __asm__ __volatile__("mov %%cr3, %0" : "=r"(val));
    return (uint32_t*)val;
}

/* Hardware Initialization */
int init_vga();
int init_keyboard();
int probe_io_ports();
int detect_memory();
int search_pci();
void init_timer(uint32_t frequency);

/* Task Management & Scheduling */
struct task_struct; 
int init_scheduler();
void task_init();
void schedule();
void list_tasks();
struct task_struct* create_task(void (*entry_point)());
extern void switch_to(uint32_t* old_esp, uint32_t new_esp);

/* Memory Management */
void init_paging();
void switch_paging(uint32_t* pd);
void vmm_map(uint32_t* pd, uint32_t virtual_addr, uint32_t physical_addr, int flags);
uint32_t* vmm_create_address_space();
uint32_t get_free_heap_memory();

/* Storage & File System */
extern void ata_init();
extern void ata_read_sector(uint32_t lba, uint8_t* buffer);
extern void ata_write_sector(uint32_t lba, uint8_t* buffer);
extern void fat16_init();
extern void fat16_list_root();

/* Kernel Entry Point Helpers */
void kernel_setup_hardware();

/* ELF Loader */
struct proc_page_tracker;

/* Networking */
void net_init(void);
void net_poll(void);
void virtio_net_irq_handler(void);

/* Global State */
extern volatile int keyboard_irq_count;
extern volatile uint8_t last_scancode_raw;
extern volatile char last_char;
extern volatile int key_event_happened;

#endif