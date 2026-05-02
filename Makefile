# ==============================================================================
# Cact Kernel Makefile
# ==============================================================================




KERN_CORE_DIR    = Cact/kernel/core
KERN_VER_DIR     = Cact/kernel/core/kern_ver
KERN_SYSCALLS_DIR    = Cact/kernel/core/syscalls
KERN_SC_PROC_DIR     = $(KERN_SYSCALLS_DIR)/proc
KERN_SC_FD_DIR       = $(KERN_SYSCALLS_DIR)/fd
KERN_SC_FILE_DIR     = $(KERN_SYSCALLS_DIR)/file
KERN_SC_PATH_DIR     = $(KERN_SYSCALLS_DIR)/path
KERN_SC_SYS_DIR      = $(KERN_SYSCALLS_DIR)/sys
KERN_SC_MM_DIR       = $(KERN_SYSCALLS_DIR)/mm
KERN_SC_IPC_DIR      = $(KERN_SYSCALLS_DIR)/ipc
KERN_SC_TIME_DIR     = $(KERN_SYSCALLS_DIR)/time
KERN_SC_USER_DIR     = $(KERN_SYSCALLS_DIR)/user
KERN_SC_NET_DIR      = $(KERN_SYSCALLS_DIR)/net
KERN_GDT_DIR     = Cact/kernel/gdt
KERN_ELF_DIR     = Cact/kernel/elf
KERN_DYNLINK_DIR = Cact/kernel/elf/dynlink
KERN_MEM_DIR     = Cact/kernel/memory
KERN_PROC_DIR    = Cact/kernel/proc
KERN_SYNC_DIR    = Cact/kernel/sync
SCHED_DIR    = Cact/kernel/proc/sched
SCHED_RS_SOURCES := $(shell find $(SCHED_DIR)/src -type f -name '*.rs' 2>/dev/null | LC_ALL=C sort)
CACT_SYNC_RS := $(shell find $(KERN_SYNC_DIR)/src -type f -name '*.rs' 2>/dev/null | LC_ALL=C sort)
SCHED_TARGET = $(SCHED_DIR)/target/i686-cact/release/libsched.a
CARGO        = cargo +nightly

# Rust memory manager
RUST_MM_DIR  = Cact/kernel/memory/rust_mm
RUST_MM_LIB  = $(RUST_MM_DIR)/target/i686-cact/release/libcact_mm.a
KERN_IDT_DIR     = Cact/kernel/idt
DRIVER_INPUT_DIR     = Cact/drivers/input
DRIVER_PS2_KBD_DIR   = Cact/drivers/input/ps_2/keyboard
DRIVER_PS2_MOUSE_DIR = Cact/drivers/input/ps_2/mouse
DRIVER_PCI_DIR        = Cact/drivers/pci
DRIVER_PCI_ENUM_DIR   = Cact/drivers/pci/enum
DRIVER_PCI_DRV_DIR    = Cact/drivers/pci/driver
DRIVER_PCI_LOADER_DIR = Cact/drivers/pci/loader
DRIVER_BLK_BLOCK_DIR  = Cact/drivers/block/blkdev
DRIVER_AHCI_DIR  = Cact/drivers/block/AHCI
DRIVER_NVME_DIR  = Cact/drivers/block/NVMe
DRIVER_BUF_DIR   = Cact/drivers/block/buf
DRIVER_USB_DIR       = Cact/drivers/usb
DRIVER_USB_XHCI_DIR  = Cact/drivers/usb/xHCI
DRIVER_USB_HID_DIR   = Cact/drivers/usb/hid
DRIVER_USB_HUB_DIR   = Cact/drivers/usb/hub
FS_VFS_DIR       = Cact/fs/vfs
FS_PIPE_DIR      = Cact/pipe
FS_DEVFS_DIR     = Cact/fs/vfs/devfs
FS_PG_DIR        = Cact/drivers/block/pagecache
FS_EXT4_DIR      = Cact/fs/ext4
FS_PROCFS_DIR    = Cact/fs/vfs/procfs
FS_MNTFS_DIR     = Cact/fs/vfs/mntfs
FS_ETCFS_DIR     = Cact/fs/vfs/etcfs
FS_TMPFS_DIR     = Cact/fs/vfs/tmpfs
FS_BINFS_DIR     = Cact/fs/vfs/binfs
NET_DIR          = Cact/net
NET_ARP_DIR      = Cact/net/arp
NET_ETH_DIR      = Cact/net/ethernet
NET_IP_DIR       = Cact/net/ip
NET_ICMP_DIR     = Cact/net/icmp
NET_UDP_DIR      = Cact/net/protocols/udp
NET_TCP_DIR      = Cact/net/protocols/tcp
NET_SOCKET_DIR   = Cact/net/socket
DRIVER_NET_DIR   = Cact/drivers/network/virtio_net
DRIVER_FB_DIR    = Cact/drivers/video/fb
DRIVER_FONT_DIR  = Cact/drivers/video/font
BUILD_DIR        = build

# Version metadata from VERSION file
CACT_VERSION    := $(shell cat VERSION 2>/dev/null || echo "unknown")
CACT_COMMIT     := $(shell git rev-parse --short HEAD 2>/dev/null || echo "no-git")
CACT_BUILD_TIME := $(shell date '+%Y-%m-%d %H:%M:%S' 2>/dev/null || echo "unknown")

VERSION_DEFS = -DCACT_VERSION=$(CACT_VERSION) \
               -DCACT_COMMIT_HASH=$(CACT_COMMIT) \
               -DCACT_BUILD_TIME="$(CACT_BUILD_TIME)"

CFLAGS = -m32 -ffreestanding -fno-pie -fno-stack-protector -nostdlib \
         -I$(KERN_CORE_DIR) \
         -I$(KERN_VER_DIR) \
		 -I$(KERN_SYSCALLS_DIR) \
         -I$(KERN_GDT_DIR) \
         -I$(KERN_ELF_DIR) \
         -I$(KERN_DYNLINK_DIR) \
         -I$(KERN_MEM_DIR) \
         -I$(KERN_PROC_DIR) \
         -I$(KERN_SYNC_DIR) \
         -I$(KERN_IDT_DIR) \
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
         -I$(FS_TMPFS_DIR) \
         -I$(FS_BINFS_DIR) \
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
      $(BUILD_DIR)/version.o \
      $(BUILD_DIR)/kernel.o \
      $(BUILD_DIR)/multiboot2.o \
      $(BUILD_DIR)/elf_loader.o \
      $(BUILD_DIR)/dynlink.o \
      $(BUILD_DIR)/idt.o \
      $(BUILD_DIR)/klib.o \
      $(BUILD_DIR)/vfs.o \
      $(BUILD_DIR)/pipe.o \
      $(BUILD_DIR)/devfs.o \
      $(BUILD_DIR)/ext4.o \
	  $(BUILD_DIR)/pagecache.o \
      $(BUILD_DIR)/procfs.o \
      $(BUILD_DIR)/mntfs.o \
      $(BUILD_DIR)/etcfs.o \
      $(BUILD_DIR)/tmpfs.o \
      $(BUILD_DIR)/binfs.o \
      $(BUILD_DIR)/pci.o \
      $(BUILD_DIR)/pci_enum.o \
      $(BUILD_DIR)/pci_driver.o \
      $(BUILD_DIR)/pci_loader.o \
	  $(BUILD_DIR)/blkdev.o \
	  $(BUILD_DIR)/ahci.o \
      $(BUILD_DIR)/nvme.o \
      $(BUILD_DIR)/buf.o \
      $(BUILD_DIR)/sc_mod.o \
      $(BUILD_DIR)/sc_validate.o \
      $(BUILD_DIR)/sc_resolve.o \
      $(BUILD_DIR)/sc_helper.o \
      $(BUILD_DIR)/sc_proc.o \
      $(BUILD_DIR)/sc_signal.o \
      $(BUILD_DIR)/sc_session.o \
      $(BUILD_DIR)/sc_fd.o \
      $(BUILD_DIR)/sc_file.o \
      $(BUILD_DIR)/sc_path.o \
      $(BUILD_DIR)/sc_sys.o \
      $(BUILD_DIR)/sc_mm.o \
      $(BUILD_DIR)/sc_ipc.o \
      $(BUILD_DIR)/sc_time.o \
      $(BUILD_DIR)/sc_user.o \
      $(BUILD_DIR)/sc_net.o \
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
      $(BUILD_DIR)/font.o \
      $(BUILD_DIR)/stack_guard.o


all: $(BUILD_DIR)/cact.iso
	@echo "--------------------------------------------------"
	@echo "Cact kernel build complete!"
	@echo "  Version: $(CACT_VERSION)"
	@echo "  Commit:  $(CACT_COMMIT)"
	@echo "  Built:   $(CACT_BUILD_TIME)"
	@echo "  Kernel:  $(BUILD_DIR)/kernel.bin"
	@echo "  Image:   $(BUILD_DIR)/cact.iso"
	@KERN_SIZE=$$(wc -c < $(BUILD_DIR)/kernel.bin); \
	 KERN_SECTORS=$$(( ($$KERN_SIZE + 511) / 512 )); \
	 echo "  Kernel size: $$KERN_SIZE bytes ($$KERN_SECTORS sectors)";
	@echo "--------------------------------------------------"


$(BUILD_DIR)/cact.iso: $(BUILD_DIR)/kernel.bin grub.cfg
	@mkdir -p $(BUILD_DIR)/isodir/boot/grub
	cp $(BUILD_DIR)/kernel.bin $(BUILD_DIR)/isodir/boot/kernel.bin
	cp grub.cfg $(BUILD_DIR)/isodir/boot/grub/grub.cfg
	grub-mkrescue -o $(BUILD_DIR)/cact.iso $(BUILD_DIR)/isodir


$(RUST_MM_LIB): FORCE
	cd $(RUST_MM_DIR) && cargo build --release

FORCE:

$(BUILD_DIR)/kernel.bin: $(OBJ) $(RUST_MM_LIB) $(SCHED_TARGET)
	ld $(LDFLAGS) -o $@ --start-group $(OBJ) $(RUST_MM_LIB) -L$(dir $(SCHED_TARGET)) -lsched --end-group


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

$(BUILD_DIR)/version.o: $(KERN_VER_DIR)/version.c $(KERN_VER_DIR)/version.h VERSION
	@mkdir -p $(BUILD_DIR)
	gcc $(CFLAGS) $(VERSION_DEFS) -c $< -o $@

$(BUILD_DIR)/kernel.o: $(KERN_CORE_DIR)/kernel.c
	@mkdir -p $(BUILD_DIR)
	gcc $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/multiboot2.o: $(KERN_CORE_DIR)/multiboot2.c
	@mkdir -p $(BUILD_DIR)
	gcc $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/sc_mod.o: $(KERN_SYSCALLS_DIR)/mod.c
	@mkdir -p $(BUILD_DIR)
	gcc $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/sc_validate.o: $(KERN_SYSCALLS_DIR)/validate.c
	@mkdir -p $(BUILD_DIR)
	gcc $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/sc_resolve.o: $(KERN_SYSCALLS_DIR)/resolve.c
	@mkdir -p $(BUILD_DIR)
	gcc $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/sc_helper.o: $(KERN_SYSCALLS_DIR)/helper.c
	@mkdir -p $(BUILD_DIR)
	gcc $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/sc_proc.o: $(KERN_SC_PROC_DIR)/proc.c
	@mkdir -p $(BUILD_DIR)
	gcc $(CFLAGS) -I$(KERN_SC_PROC_DIR) -c $< -o $@

$(BUILD_DIR)/sc_signal.o: $(KERN_SC_PROC_DIR)/signal.c
	@mkdir -p $(BUILD_DIR)
	gcc $(CFLAGS) -I$(KERN_SC_PROC_DIR) -c $< -o $@

$(BUILD_DIR)/sc_session.o: $(KERN_SC_PROC_DIR)/session.c
	@mkdir -p $(BUILD_DIR)
	gcc $(CFLAGS) -I$(KERN_SC_PROC_DIR) -c $< -o $@

$(BUILD_DIR)/sc_fd.o: $(KERN_SC_FD_DIR)/fd.c
	@mkdir -p $(BUILD_DIR)
	gcc $(CFLAGS) -I$(KERN_SC_FD_DIR) -c $< -o $@

$(BUILD_DIR)/sc_file.o: $(KERN_SC_FILE_DIR)/file.c
	@mkdir -p $(BUILD_DIR)
	gcc $(CFLAGS) -I$(KERN_SC_FILE_DIR) -c $< -o $@

$(BUILD_DIR)/sc_path.o: $(KERN_SC_PATH_DIR)/path.c
	@mkdir -p $(BUILD_DIR)
	gcc $(CFLAGS) -I$(KERN_SC_PATH_DIR) -c $< -o $@

$(BUILD_DIR)/sc_sys.o: $(KERN_SC_SYS_DIR)/sys.c
	@mkdir -p $(BUILD_DIR)
	gcc $(CFLAGS) -I$(KERN_SC_SYS_DIR) -c $< -o $@

$(BUILD_DIR)/sc_mm.o: $(KERN_SC_MM_DIR)/mm.c
	@mkdir -p $(BUILD_DIR)
	gcc $(CFLAGS) -I$(KERN_SC_MM_DIR) -c $< -o $@

$(BUILD_DIR)/sc_ipc.o: $(KERN_SC_IPC_DIR)/ipc.c
	@mkdir -p $(BUILD_DIR)
	gcc $(CFLAGS) -I$(KERN_SC_IPC_DIR) -c $< -o $@

$(BUILD_DIR)/sc_time.o: $(KERN_SC_TIME_DIR)/time.c
	@mkdir -p $(BUILD_DIR)
	gcc $(CFLAGS) -I$(KERN_SC_TIME_DIR) -c $< -o $@

$(BUILD_DIR)/sc_user.o: $(KERN_SC_USER_DIR)/user.c
	@mkdir -p $(BUILD_DIR)
	gcc $(CFLAGS) -I$(KERN_SC_USER_DIR) -c $< -o $@

$(BUILD_DIR)/sc_net.o: $(KERN_SC_NET_DIR)/net.c
	@mkdir -p $(BUILD_DIR)
	gcc $(CFLAGS) -I$(KERN_SC_NET_DIR) -c $< -o $@

$(BUILD_DIR)/idt.o: $(KERN_IDT_DIR)/idt.c
	@mkdir -p $(BUILD_DIR)
	gcc $(CFLAGS) -c $< -o $@

$(SCHED_TARGET): $(SCHED_RS_SOURCES) $(CACT_SYNC_RS) $(SCHED_DIR)/Cargo.toml $(SCHED_DIR)/targets/i686-cact.json $(KERN_SYNC_DIR)/Cargo.toml $(KERN_SYNC_DIR)/targets/i686-cact.json
	cd $(SCHED_DIR) && $(CARGO) build --release \
		-Z json-target-spec \
		-Z build-std=core,compiler_builtins \
		-Z build-std-features=compiler-builtins-mem 2>&1

$(BUILD_DIR)/elf_loader.o: $(KERN_ELF_DIR)/elf_loader.c
	@mkdir -p $(BUILD_DIR)
	gcc $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/dynlink.o: $(KERN_DYNLINK_DIR)/dynlink.c
	@mkdir -p $(BUILD_DIR)
	gcc $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/klib.o: $(KERN_CORE_DIR)/klib.c
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

$(BUILD_DIR)/tmpfs.o: $(FS_TMPFS_DIR)/tmpfs.c
	@mkdir -p $(BUILD_DIR)
	gcc $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/binfs.o: $(FS_BINFS_DIR)/binfs.c
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

$(BUILD_DIR)/stack_guard.o: $(KERN_CORE_DIR)/stack_guard.c
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
	cd $(SCHED_DIR) && cargo +nightly clean
	cd $(KERN_SYNC_DIR) && cargo +nightly clean
	cd $(RUST_MM_DIR) && cargo clean

.PHONY: all clean sched FORCE
.PHONY: sched
sched: $(SCHED_TARGET)