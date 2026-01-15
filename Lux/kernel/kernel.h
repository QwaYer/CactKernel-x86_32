#ifndef KERNEL_H
#define KERNEL_H

#define VIDEO_ADDRESS 0xb8000
#define MAX_ROWS 25
#define MAX_COLS 80
#define WHITE_ON_BLACK 0x0f

extern void port_byte_out(unsigned short port, unsigned char data);
extern unsigned char port_byte_in(unsigned short port);
extern void port_long_out(unsigned short port, unsigned int data);
extern unsigned int port_long_in(unsigned short port);

void kprint(char* message);
void kprint_at(char* message, int x, int y);
void clear_screen();
void scroll();
void update_hardware_cursor(int x, int y);

void init_pic();
int init_idt();
void set_idt_gate(int n, unsigned int handler);
void keyboard_handler(); 

int init_vga();
int init_keyboard();
int probe_io_ports();
int detect_memory();
int search_pci();
int init_scheduler();

int strcmp(char* s1, char* s2);
void execute_command(char* input);

extern unsigned char keyboard_map[128];

#endif