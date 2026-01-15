BOOT_DIR = Lumen/boot
KERN_DIR = Lux/kernel
BUILD_DIR = build

CFLAGS = -m32 -ffreestanding -fno-pie -fno-stack-protector -nostdlib -c

LDFLAGS = -m elf_i386 -Ttext 0x7e00 --oformat binary

all: $(BUILD_DIR)/luxos.img

$(BUILD_DIR)/luxos.img: $(BUILD_DIR)/boot.bin $(BUILD_DIR)/kernel.bin
	cat $^ > $@
	truncate -s 20480 $@

$(BUILD_DIR)/boot.bin: $(BOOT_DIR)/boot.asm
	nasm -f bin $< -o $@

$(BUILD_DIR)/kernel.bin: $(BUILD_DIR)/kernel_entry.o $(BUILD_DIR)/io.o $(BUILD_DIR)/interrupt.o $(BUILD_DIR)/kernel.o
	ld $(LDFLAGS) -o $@ $^

$(BUILD_DIR)/kernel_entry.o: $(KERN_DIR)/kernel.asm
	nasm -f elf32 $< -o $@

$(BUILD_DIR)/io.o: $(KERN_DIR)/io.asm
	nasm -f elf32 $< -o $@

$(BUILD_DIR)/interrupt.o: $(KERN_DIR)/interrupt.asm
	nasm -f elf32 $< -o $@

$(BUILD_DIR)/kernel.o: $(KERN_DIR)/kernel.c
	gcc $(CFLAGS) $< -o $@

clean:
	rm -rf $(BUILD_DIR)/*.bin $(BUILD_DIR)/*.o $(BUILD_DIR)/*.img