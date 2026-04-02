# ==============================================================================
# Lux Kernel Makefile
# ==============================================================================

KERN_CORE_DIR    = Lux/kernel/core
KERN_SYSCALLS_DIR    = Lux/kernel/core/syscalls
KERN_GDT_DIR     = Lux/kernel/gdt
KERN_ELF_DIR     = Lux/kernel/elf
KERN_DYNLINK_DIR = Lux/kernel/elf/dynlink
KERN_SHELL_DIR   = Lux/kernel/shell
KERN_CMDS_DIR    = Lux/kernel/shell/commands
KERN_MEM_DIR     = Lux/kernel/memory
KERN_PROCMM_DIR  = Lux/kernel/memory/proc_mm
KERN_SLABMM_DIR  = Lux/kernel/memory/slab_mm
KERN_PF_DIR      = Lux/kernel/memory/page_fault
KERN_SWAP_DIR    = Lux/kernel/memory/swap_mm
KERN_OOM_DIR     = Lux/kernel/memory/oom
KERN_MMAP_DIR    = Lux/kernel/memory/mmap_mm
KERN_SHM_DIR     = Lux/kernel/memory/shm
KERN_PROC_DIR    = Lux/kernel/proc
KERN_SYNC_DIR    = Lux/kernel/sync
KERN_IDT_DIR     = Lux/kernel/idt
KERN_LIBC_DIR    = Lux/libc
DRIVER_INPUT_DIR     = Lux/drivers/input
DRIVER_PS2_KBD_DIR   = Lux/drivers/input/ps_2/keyboard
DRIVER_PS2_MOUSE_DIR = Lux/drivers/input/ps_2/mouse
DRIVER_PCI_DIR        = Lux/drivers/pci
DRIVER_PCI_ENUM_DIR   = Lux/drivers/pci/enum
DRIVER_PCI_DRV_DIR    = Lux/drivers/pci/driver
DRIVER_PCI_LOADER_DIR = Lux/drivers/pci/loader
DRIVER_BLK_BLOCK_DIR  = Lux/drivers/block/blkdev
DRIVER_AHCI_DIR  = Lux/drivers/block/AHCI
DRIVER_NVME_DIR  = Lux/drivers/block/NVMe
DRIVER_BUF_DIR   = Lux/drivers/block/buf
DRIVER_USB_DIR       = Lux/drivers/usb
DRIVER_USB_XHCI_DIR  = Lux/drivers/usb/xHCI
DRIVER_USB_HID_DIR   = Lux/drivers/usb/hid
DRIVER_USB_HUB_DIR   = Lux/drivers/usb/hub
FS_VFS_DIR       = Lux/fs/vfs
FS_PIPE_DIR      = Lux/pipe
FS_DEVFS_DIR     = Lux/fs/vfs/devfs
FS_PG_DIR        = Lux/drivers/block/pagecache
FS_EXT4_DIR      = Lux/fs/ext4
FS_PROCFS_DIR    = Lux/fs/vfs/procfs
FS_MNTFS_DIR     = Lux/fs/vfs/mntfs
FS_ETCFS_DIR     = Lux/fs/vfs/etcfs
NET_DIR          = Lux/net
NET_ARP_DIR      = Lux/net/arp
NET_ETH_DIR      = Lux/net/ethernet
NET_IP_DIR       = Lux/net/ip
NET_ICMP_DIR     = Lux/net/icmp
NET_UDP_DIR      = Lux/net/protocols/udp
NET_TCP_DIR      = Lux/net/protocols/tcp
NET_SOCKET_DIR   = Lux/net/socket
DRIVER_NET_DIR   = Lux/drivers/network/virtio_net
DRIVER_FB_DIR    = Lux/drivers/video/fb
DRIVER_FONT_DIR  = Lux/drivers/video/font
BUILD_DIR        = build


CFLAGS = -m32 -ffreestanding -fno-pie -fno-stack-protector -nostdlib \
         -I$(KERN_CORE_DIR) \
		 -I$(KERN_SYSCALLS_DIR) \
         -I$(KERN_GDT_DIR) \
         -I$(KERN_ELF_DIR) \
         -I$(KERN_DYNLINK_DIR) \
         -I$(KERN_SHELL_DIR) \
         -I$(KERN_CMDS_DIR) \
         -I$(KERN_MEM_DIR) \
         -I$(KERN_PF_DIR) \
         -I$(KERN_PROCMM_DIR) \
         -I$(KERN_SLABMM_DIR) \
         -I$(KERN_SWAP_DIR) \
         -I$(KERN_OOM_DIR) \
         -I$(KERN_MMAP_DIR) \
         -I$(KERN_SHM_DIR) \
         -I$(KERN_PROC_DIR) \
         -I$(KERN_SYNC_DIR) \
         -I$(KERN_IDT_DIR) \
         -I$(KERN_LIBC_DIR) \
         -I$(DRIVER_INPUT_DIR) \
         -I$(DRIVER_PS2_KBD_DIR) \
         -I$(DRIVER_PS2_MOUSE_DIR) \
         -I$(DRIVER_PCI_DIR) \
         -I$(DRIVER_PCI_ENUM_DIR) \
         -I$(DRIVER_PCI_DRV_DIR) \
         -I$(DRIVER_PCI_LOADER_DIR) \
		 -I$(DRIVER_BLK_BLOCK_DIR) \
		 -I$(DRIVER_AHCI_DIR) \
         -I$(DRIVER_NVME_DIR) \
         -I$(DRIVER_BUF_DIR) \
         -I$(DRIVER_USB_DIR) \
         -I$(DRIVER_USB_XHCI_DIR) \
         -I$(DRIVER_USB_HID_DIR) \
         -I$(DRIVER_USB_HUB_DIR) \
         -I$(FS_VFS_DIR) \
         -I$(FS_PIPE_DIR) \
         -I$(FS_DEVFS_DIR) \
         -I$(FS_EXT4_DIR) \
		 -I$(FS_PG_DIR) \
         -I$(FS_PROCFS_DIR) \
         -I$(FS_MNTFS_DIR) \
         -I$(FS_ETCFS_DIR) \
         -I$(NET_DIR) \
         -I$(NET_ARP_DIR) \
         -I$(NET_ETH_DIR) \
         -I$(NET_IP_DIR) \
         -I$(NET_ICMP_DIR) \
         -I$(NET_UDP_DIR) \
         -I$(NET_TCP_DIR) \
         -I$(NET_SOCKET_DIR) \
         -I$(DRIVER_NET_DIR) \
         -I$(DRIVER_FB_DIR) \
         -I$(DRIVER_FONT_DIR) \
         -Wall

LDFLAGS = -m elf_i386 -T linker.ld -z noexecstack


OBJ = $(BUILD_DIR)/kernel_entry.o \
      $(BUILD_DIR)/gdt_asm.o \
      $(BUILD_DIR)/task_asm.o \
      $(BUILD_DIR)/mm.o \
      $(BUILD_DIR)/interrupt.o \
      $(BUILD_DIR)/io.o \
      $(BUILD_DIR)/gdt.o \
      $(BUILD_DIR)/kernel.o \
      $(BUILD_DIR)/memory.o \
      $(BUILD_DIR)/memory_cow.o \
      $(BUILD_DIR)/proc_mm.o \
      $(BUILD_DIR)/slab.o \
      $(BUILD_DIR)/page_fault.o \
      $(BUILD_DIR)/swap.o \
      $(BUILD_DIR)/oom.o \
      $(BUILD_DIR)/mmap.o \
      $(BUILD_DIR)/shm.o \
      $(BUILD_DIR)/task.o \
      $(BUILD_DIR)/elf_loader.o \
      $(BUILD_DIR)/dynlink.o \
      $(BUILD_DIR)/sync.o \
      $(BUILD_DIR)/idt.o \
      $(BUILD_DIR)/shell.o \
      $(BUILD_DIR)/commands.o \
      $(BUILD_DIR)/libc.o \
      $(BUILD_DIR)/vfs.o \
      $(BUILD_DIR)/pipe.o \
      $(BUILD_DIR)/devfs.o \
      $(BUILD_DIR)/ext4.o \
	  $(BUILD_DIR)/pagecache.o \
      $(BUILD_DIR)/procfs.o \
      $(BUILD_DIR)/mntfs.o \
      $(BUILD_DIR)/etcfs.o \
      $(BUILD_DIR)/pci.o \
      $(BUILD_DIR)/pci_enum.o \
      $(BUILD_DIR)/pci_driver.o \
      $(BUILD_DIR)/pci_loader.o \
	  $(BUILD_DIR)/blkdev.o \
	  $(BUILD_DIR)/ahci.o \
      $(BUILD_DIR)/nvme.o \
      $(BUILD_DIR)/buf.o \
      $(BUILD_DIR)/syscall.o \
      $(BUILD_DIR)/keyboard.o \
      $(BUILD_DIR)/mouse.o \
      $(BUILD_DIR)/ps_2_keyboard.o \
      $(BUILD_DIR)/ps_2_mouse.o \
      $(BUILD_DIR)/usb.o \
      $(BUILD_DIR)/xhci.o \
      $(BUILD_DIR)/usb_hid.o \
      $(BUILD_DIR)/usb_hub.o \
      $(BUILD_DIR)/net.o \
      $(BUILD_DIR)/ethernet.o \
      $(BUILD_DIR)/arp.o \
      $(BUILD_DIR)/ip.o \
      $(BUILD_DIR)/icmp.o \
      $(BUILD_DIR)/udp.o \
      $(BUILD_DIR)/tcp.o \
      $(BUILD_DIR)/ksocket.o \
      $(BUILD_DIR)/virtio_net.o \
      $(BUILD_DIR)/fb.o \
      $(BUILD_DIR)/font.o


all: $(BUILD_DIR)/lux.iso
	@echo "--------------------------------------------------"
	@echo "Lux kernel build complete!"
	@echo "  Kernel:  $(BUILD_DIR)/kernel.bin"
	@echo "  Image:   $(BUILD_DIR)/lux.iso"
	@KERN_SIZE=$$(wc -c < $(BUILD_DIR)/kernel.bin); \
	 KERN_SECTORS=$$(( ($$KERN_SIZE + 511) / 512 )); \
	 echo "  Kernel size: $$KERN_SIZE bytes ($$KERN_SECTORS sectors)";
	@echo "--------------------------------------------------"


$(BUILD_DIR)/lux.iso: $(BUILD_DIR)/kernel.bin grub.cfg
	@mkdir -p $(BUILD_DIR)/isodir/boot/grub
	cp $(BUILD_DIR)/kernel.bin $(BUILD_DIR)/isodir/boot/kernel.bin
	cp grub.cfg $(BUILD_DIR)/isodir/boot/grub/grub.cfg
	grub-mkrescue -o $(BUILD_DIR)/lux.iso $(BUILD_DIR)/isodir


$(BUILD_DIR)/kernel.bin: $(OBJ)
	ld $(LDFLAGS) -o $@ $^


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


$(BUILD_DIR)/gdt.o: $(KERN_GDT_DIR)/gdt.c
	@mkdir -p $(BUILD_DIR)
	gcc $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/kernel.o: $(KERN_CORE_DIR)/kernel.c
	@mkdir -p $(BUILD_DIR)
	gcc $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/syscall.o: $(KERN_SYSCALLS_DIR)/syscall.c
	@mkdir -p $(BUILD_DIR)
	gcc $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/idt.o: $(KERN_IDT_DIR)/idt.c
	@mkdir -p $(BUILD_DIR)
	gcc $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/memory.o: $(KERN_MEM_DIR)/memory.c
	@mkdir -p $(BUILD_DIR)
	gcc $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/page_fault.o: $(KERN_PF_DIR)/page_fault.c
	@mkdir -p $(BUILD_DIR)
	gcc $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/memory_cow.o: $(KERN_MEM_DIR)/memory_cow.c
	@mkdir -p $(BUILD_DIR)
	gcc $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/proc_mm.o: $(KERN_PROCMM_DIR)/proc_mm.c
	@mkdir -p $(BUILD_DIR)
	gcc $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/slab.o: $(KERN_SLABMM_DIR)/slab.c
	@mkdir -p $(BUILD_DIR)
	gcc $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/swap.o: $(KERN_SWAP_DIR)/swap.c
	@mkdir -p $(BUILD_DIR)
	gcc $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/oom.o: $(KERN_OOM_DIR)/oom.c
	@mkdir -p $(BUILD_DIR)
	gcc $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/mmap.o: $(KERN_MMAP_DIR)/mmap.c
	@mkdir -p $(BUILD_DIR)
	gcc $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/shm.o: $(KERN_SHM_DIR)/shm.c
	@mkdir -p $(BUILD_DIR)
	gcc $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/task.o: $(KERN_PROC_DIR)/task.c
	@mkdir -p $(BUILD_DIR)
	gcc $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/sync.o: $(KERN_SYNC_DIR)/sync.c
	@mkdir -p $(BUILD_DIR)
	gcc $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/elf_loader.o: $(KERN_ELF_DIR)/elf_loader.c
	@mkdir -p $(BUILD_DIR)
	gcc $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/dynlink.o: $(KERN_DYNLINK_DIR)/dynlink.c
	@mkdir -p $(BUILD_DIR)
	gcc $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/shell.o: $(KERN_SHELL_DIR)/shell.c
	@mkdir -p $(BUILD_DIR)
	gcc $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/commands.o: $(KERN_CMDS_DIR)/commands.c
	@mkdir -p $(BUILD_DIR)
	gcc $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/libc.o: $(KERN_LIBC_DIR)/libc.c
	@mkdir -p $(BUILD_DIR)
	gcc $(CFLAGS) -c $< -o $@


$(BUILD_DIR)/vfs.o: $(FS_VFS_DIR)/vfs.c
	@mkdir -p $(BUILD_DIR)
	gcc $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/pipe.o: $(FS_PIPE_DIR)/pipe.c
	@mkdir -p $(BUILD_DIR)
	gcc $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/devfs.o: $(FS_DEVFS_DIR)/devfs.c
	@mkdir -p $(BUILD_DIR)
	gcc $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/pagecache.o: $(FS_PG_DIR)/pagecache.c
	@mkdir -p $(BUILD_DIR)
	gcc $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/ext4.o: $(FS_EXT4_DIR)/ext4.c
	@mkdir -p $(BUILD_DIR)
	gcc $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/procfs.o: $(FS_PROCFS_DIR)/procfs.c
	@mkdir -p $(BUILD_DIR)
	gcc $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/mntfs.o: $(FS_MNTFS_DIR)/mntfs.c
	@mkdir -p $(BUILD_DIR)
	gcc $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/etcfs.o: $(FS_ETCFS_DIR)/etcfs.c
	@mkdir -p $(BUILD_DIR)
	gcc $(CFLAGS) -c $< -o $@


$(BUILD_DIR)/pci.o: $(DRIVER_PCI_DIR)/pci.c
	@mkdir -p $(BUILD_DIR)
	gcc $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/pci_enum.o: $(DRIVER_PCI_ENUM_DIR)/pci_enum.c
	@mkdir -p $(BUILD_DIR)
	gcc $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/pci_driver.o: $(DRIVER_PCI_DRV_DIR)/pci_driver.c
	@mkdir -p $(BUILD_DIR)
	gcc $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/pci_loader.o: $(DRIVER_PCI_LOADER_DIR)/pci_loader.c
	@mkdir -p $(BUILD_DIR)
	gcc $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/blkdev.o: $(DRIVER_BLK_BLOCK_DIR)/blkdev.c
	@mkdir -p $(BUILD_DIR)
	gcc $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/ahci.o: $(DRIVER_AHCI_DIR)/ahci.c
	@mkdir -p $(BUILD_DIR)
	gcc $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/nvme.o: $(DRIVER_NVME_DIR)/nvme.c
	@mkdir -p $(BUILD_DIR)
	gcc $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/buf.o: $(DRIVER_BUF_DIR)/buf.c
	@mkdir -p $(BUILD_DIR)
	gcc $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/keyboard.o: $(DRIVER_INPUT_DIR)/keyboard.c
	@mkdir -p $(BUILD_DIR)
	gcc $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/mouse.o: $(DRIVER_INPUT_DIR)/mouse.c
	@mkdir -p $(BUILD_DIR)
	gcc $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/ps_2_keyboard.o: $(DRIVER_PS2_KBD_DIR)/ps_2_keyboard.c
	@mkdir -p $(BUILD_DIR)
	gcc $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/ps_2_mouse.o: $(DRIVER_PS2_MOUSE_DIR)/ps_2_mouse.c
	@mkdir -p $(BUILD_DIR)
	gcc $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/usb.o: $(DRIVER_USB_DIR)/usb.c
	@mkdir -p $(BUILD_DIR)
	gcc $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/xhci.o: $(DRIVER_USB_XHCI_DIR)/xhci.c
	@mkdir -p $(BUILD_DIR)
	gcc $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/usb_hid.o: $(DRIVER_USB_HID_DIR)/usb_hid.c
	@mkdir -p $(BUILD_DIR)
	gcc $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/usb_hub.o: $(DRIVER_USB_HUB_DIR)/usb_hub.c
	@mkdir -p $(BUILD_DIR)
	gcc $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/fb.o: $(DRIVER_FB_DIR)/fb.c
	@mkdir -p $(BUILD_DIR)
	gcc $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/font.o: $(DRIVER_FONT_DIR)/font.c
	@mkdir -p $(BUILD_DIR)
	gcc $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/virtio_net.o: $(DRIVER_NET_DIR)/virtio_net.c
	@mkdir -p $(BUILD_DIR)
	gcc $(CFLAGS) -c $< -o $@


$(BUILD_DIR)/net.o: $(NET_DIR)/net.c
	@mkdir -p $(BUILD_DIR)
	gcc $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/ethernet.o: $(NET_ETH_DIR)/ethernet.c
	@mkdir -p $(BUILD_DIR)
	gcc $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/arp.o: $(NET_ARP_DIR)/arp.c
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

$(BUILD_DIR)/ksocket.o: $(NET_SOCKET_DIR)/socket.c
	@mkdir -p $(BUILD_DIR)
	gcc $(CFLAGS) -c $< -o $@


clean:
	rm -rf $(BUILD_DIR)

.PHONY: all clean