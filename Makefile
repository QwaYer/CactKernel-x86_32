# ==============================================================================
# LuxOS Makefile 
# ==============================================================================

BOOT_DIR         = Lumen/boot
KERN_CORE_DIR    = Lux/kernel/core
KERN_GDT_DIR     = Lux/kernel/gdt
KERN_ELF_DIR     = Lux/kernel/elf
KERN_SHELL_DIR   = Lux/kernel/shell
KERN_MEM_DIR     = Lux/kernel/memory
KERN_PROC_DIR    = Lux/kernel/proc
KERN_IDT_DIR     = Lux/kernel/idt
KERN_LIBC_DIR    = Lux/libc
DRIVER_INPUT_DIR = Lux/drivers/input
DRIVER_BLOCK_DIR = Lux/drivers/block
FS_VFS_DIR       = Lux/fs/vfs
FS_EXT4_DIR      = Lux/fs/ext4
NET_DIR          = Lux/net
NET_ETH_DIR      = Lux/net/ethernet
NET_IP_DIR       = Lux/net/ip
NET_ICMP_DIR     = Lux/net/icmp
NET_UDP_DIR      = Lux/net/protocols/udp
NET_TCP_DIR      = Lux/net/protocols/tcp
DRIVER_NET_DIR   = Lux/drivers/network
BUILD_DIR        = build

# ------------------------------------------------------------------------------
# Флаги компилятора
# ------------------------------------------------------------------------------

CFLAGS = -m32 -ffreestanding -fno-pie -fno-stack-protector -nostdlib \
         -I$(KERN_CORE_DIR) \
         -I$(KERN_GDT_DIR) \
         -I$(KERN_ELF_DIR) \
         -I$(KERN_SHELL_DIR) \
         -I$(KERN_MEM_DIR) \
         -I$(KERN_PROC_DIR) \
         -I$(KERN_IDT_DIR) \
         -I$(KERN_LIBC_DIR) \
         -I$(DRIVER_INPUT_DIR) \
         -I$(DRIVER_BLOCK_DIR) \
         -I$(FS_VFS_DIR) \
         -I$(FS_EXT4_DIR) \
         -I$(NET_DIR) \
         -I$(NET_ETH_DIR) \
         -I$(NET_IP_DIR) \
         -I$(NET_ICMP_DIR) \
         -I$(NET_UDP_DIR) \
         -I$(NET_TCP_DIR) \
         -I$(DRIVER_NET_DIR) \
         -Wall

LDFLAGS = -m elf_i386 -Ttext 0x10000 --oformat binary

# ------------------------------------------------------------------------------
# Объектные файлы ядра
# ------------------------------------------------------------------------------

OBJ = $(BUILD_DIR)/kernel_entry.o \
      $(BUILD_DIR)/gdt_asm.o \
      $(BUILD_DIR)/task_asm.o \
      $(BUILD_DIR)/gdt.o \
      $(BUILD_DIR)/kernel.o \
      $(BUILD_DIR)/shell.o \
      $(BUILD_DIR)/libc.o \
      $(BUILD_DIR)/memory.o \
      $(BUILD_DIR)/task.o \
      $(BUILD_DIR)/vfs.o \
      $(BUILD_DIR)/ext4.o \
      $(BUILD_DIR)/ata.o \
      $(BUILD_DIR)/syscall.o \
      $(BUILD_DIR)/elf_loader.o \
      $(BUILD_DIR)/interrupt.o \
      $(BUILD_DIR)/io.o \
      $(BUILD_DIR)/mm.o \
      $(BUILD_DIR)/keyboard.o \
      $(BUILD_DIR)/idt.o \
      $(BUILD_DIR)/net.o \
      $(BUILD_DIR)/ethernet.o \
      $(BUILD_DIR)/arp.o \
      $(BUILD_DIR)/ip.o \
      $(BUILD_DIR)/icmp.o \
      $(BUILD_DIR)/udp.o \
      $(BUILD_DIR)/tcp.o \
      $(BUILD_DIR)/virtio_net.o


all: $(BUILD_DIR)/lux.img
	@echo "--------------------------------------------------"
	@echo "Сборка ядра lux завершена успешно!"
	@echo "  Stage 1: $(BUILD_DIR)/boot.bin"
	@echo "  Stage 2: $(BUILD_DIR)/stage2.bin"
	@echo "  Ядро:    $(BUILD_DIR)/kernel.bin"
	@echo "  Образ:   $(BUILD_DIR)/lux.img"
	@KERN_SIZE=$$(wc -c < $(BUILD_DIR)/kernel.bin); \
	 KERN_SECTORS=$$(( ($$KERN_SIZE + 511) / 512 )); \
	 echo "  Размер ядра: $$KERN_SIZE байт ($$KERN_SECTORS секторов)"; 
	@echo "--------------------------------------------------"

# ------------------------------------------------------------------------------
# Сборка образа диска: Stage1 + Stage2 + Kernel
# ------------------------------------------------------------------------------

$(BUILD_DIR)/lux.img: $(BUILD_DIR)/boot.bin $(BUILD_DIR)/stage2.bin $(BUILD_DIR)/kernel.bin
	cat $^ > $@
	truncate -s 10M $@

# ------------------------------------------------------------------------------
# Stage 1 — первичный загрузчик (512 байт, грузит Stage 2 в 0x7E00)
# ------------------------------------------------------------------------------

$(BUILD_DIR)/boot.bin: $(BOOT_DIR)/boot.asm
	@mkdir -p $(BUILD_DIR)
	nasm -f bin $< -o $@
	@SIZE=$$(wc -c < $@); \
	 if [ $$SIZE -ne 512 ]; then \
	   echo "ОШИБКА: boot.bin должен быть ровно 512 байт, получилось $$SIZE"; exit 1; \
	 fi

# ------------------------------------------------------------------------------
# Stage 2 — вторичный загрузчик (16 КБ, грузит ядро в 0x10000)
# ------------------------------------------------------------------------------

$(BUILD_DIR)/stage2.bin: $(BOOT_DIR)/stage2.asm
	@mkdir -p $(BUILD_DIR)
	nasm -f bin $< -o $@
	@SIZE=$$(wc -c < $@); \
	 if [ $$SIZE -ne 16384 ]; then \
	   echo "ОШИБКА: stage2.bin должен быть ровно 16384 байт (32 сектора), получилось $$SIZE"; exit 1; \
	 fi

# ------------------------------------------------------------------------------
# Ядро — линкуется на 0x10000
# ------------------------------------------------------------------------------

$(BUILD_DIR)/kernel.bin: $(OBJ)
	ld $(LDFLAGS) -o $@ $^

# ------------------------------------------------------------------------------
# Правила для ASM-файлов
# ------------------------------------------------------------------------------

$(BUILD_DIR)/kernel_entry.o: $(KERN_CORE_DIR)/kernel.asm
	@mkdir -p $(BUILD_DIR)
	nasm -f elf32 $< -o $@

$(BUILD_DIR)/gdt_asm.o: $(KERN_GDT_DIR)/gdt.asm
	@mkdir -p $(BUILD_DIR)
	nasm -f elf32 $< -o $@

$(BUILD_DIR)/task_asm.o: $(KERN_PROC_DIR)/task.asm
	@mkdir -p $(BUILD_DIR)
	nasm -f elf32 $< -o $@

$(BUILD_DIR)/interrupt.o: $(KERN_CORE_DIR)/interrupt.asm
	@mkdir -p $(BUILD_DIR)
	nasm -f elf32 $< -o $@

$(BUILD_DIR)/io.o: $(DRIVER_INPUT_DIR)/io.asm
	@mkdir -p $(BUILD_DIR)
	nasm -f elf32 $< -o $@

$(BUILD_DIR)/mm.o: $(KERN_MEM_DIR)/mm.asm
	@mkdir -p $(BUILD_DIR)
	nasm -f elf32 $< -o $@

# ------------------------------------------------------------------------------
# Правила для C-файлов
# ------------------------------------------------------------------------------

$(BUILD_DIR)/gdt.o: $(KERN_GDT_DIR)/gdt.c
	@mkdir -p $(BUILD_DIR)
	gcc $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/kernel.o: $(KERN_CORE_DIR)/kernel.c
	@mkdir -p $(BUILD_DIR)
	gcc $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/shell.o: $(KERN_SHELL_DIR)/shell.c
	@mkdir -p $(BUILD_DIR)
	gcc $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/syscall.o: $(KERN_CORE_DIR)/syscall.c
	@mkdir -p $(BUILD_DIR)
	gcc $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/elf_loader.o: $(KERN_ELF_DIR)/elf_loader.c
	@mkdir -p $(BUILD_DIR)
	gcc $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/libc.o: $(KERN_LIBC_DIR)/libc.c
	@mkdir -p $(BUILD_DIR)
	gcc $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/memory.o: $(KERN_MEM_DIR)/memory.c
	@mkdir -p $(BUILD_DIR)
	gcc $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/task.o: $(KERN_PROC_DIR)/task.c
	@mkdir -p $(BUILD_DIR)
	gcc $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/vfs.o: $(FS_VFS_DIR)/vfs.c
	@mkdir -p $(BUILD_DIR)
	gcc $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/ext4.o: $(FS_EXT4_DIR)/ext4.c
	@mkdir -p $(BUILD_DIR)
	gcc $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/ata.o: $(DRIVER_BLOCK_DIR)/ata.c
	@mkdir -p $(BUILD_DIR)
	gcc $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/idt.o: $(KERN_IDT_DIR)/idt.c
	@mkdir -p $(BUILD_DIR)
	gcc $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/keyboard.o: $(DRIVER_INPUT_DIR)/keyboard.c
	@mkdir -p $(BUILD_DIR)
	gcc $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/net.o: $(NET_DIR)/net.c
	@mkdir -p $(BUILD_DIR)
	gcc $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/ethernet.o: $(NET_ETH_DIR)/ethernet.c
	@mkdir -p $(BUILD_DIR)
	gcc $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/arp.o: $(NET_DIR)/arp.c
	@mkdir -p $(BUILD_DIR)
	gcc $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/ip.o: $(NET_IP_DIR)/ip.c
	@mkdir -p $(BUILD_DIR)
	gcc $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/icmp.o: $(NET_ICMP_DIR)/icmp.c
	@mkdir -p $(BUILD_DIR)
	gcc $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/udp.o: $(NET_UDP_DIR)/udp.c
	@mkdir -p $(BUILD_DIR)
	gcc $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/tcp.o: $(NET_TCP_DIR)/tcp.c
	@mkdir -p $(BUILD_DIR)
	gcc $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/virtio_net.o: $(DRIVER_NET_DIR)/virtio_net.c
	@mkdir -p $(BUILD_DIR)
	gcc $(CFLAGS) -c $< -o $@

# ------------------------------------------------------------------------------

clean:
	rm -rf $(BUILD_DIR)

.PHONY: all clean