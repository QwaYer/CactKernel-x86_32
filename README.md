# 🌵 CactKernel/x86_32

<p align="center">
  <img src="https://img.shields.io/badge/version-0.9.0-green.svg?style=for-the-badge" alt="Version: 0.9.0">
  <img src="https://img.shields.io/badge/license-GPLv3-blue.svg?style=for-the-badge" alt="License: GPLv3">
  <img src="https://img.shields.io/badge/arch-i686-red.svg?style=for-the-badge" alt="Arch: i686">
  <img src="https://img.shields.io/badge/language-C%2FRust%2FASM-orange.svg?style=for-the-badge" alt="Language: C/Rust/ASM">
  <img src="https://img.shields.io/badge/boot-Multiboot2-purple.svg?style=for-the-badge" alt="Multiboot2">
  <img src="https://img.shields.io/badge/output-cact.iso-0369a1.svg?style=for-the-badge" alt="cact.iso">
  <img src="https://img.shields.io/badge/status-pre--1.0-yellow.svg?style=for-the-badge" alt="pre-1.0">
</p>

<p align="center">
  Гибридное монолитное ядро для архитектуры i686.<br>
  Низкоуровневые интерфейсы на <strong>C</strong> и <strong>NASM ASM</strong>, менеджер памяти и планировщик — на <strong>Rust</strong>.
</p>

---

## 📊 Stats

| | |
|---|---|
| **Syscalls** | 73 |
| **CPU ISRs** | 32 |
| **Physical mem range** | 128 MB |
| **xHCI driver** | ~32 KiB |
| **MLFQ levels** | 4 |
| **ext4 driver** | ~40 KiB |

---

## 🔨 Building

**Requirements:**

| Tool | Notes |
|------|-------|
| `gcc -m32` | With multilib — `gcc-multilib` on Ubuntu/Debian |
| `nasm` | For `.asm` entry/interrupt stubs |
| `ld (elf_i386)` | GNU binutils linker |
| `cargo nightly` | For `cact_mm` and `sched` Rust crates |
| `grub2-mkrescue` | Builds bootable `cact.iso` |
| `xorriso` | Required by grub2-mkrescue |

```sh
# Full build → build/cact.iso + build/kernel.bin
make

# Build Rust scheduler only
make sched

# Clean everything (incl. Rust targets)
make clean
```

**Build output:**
```
--------------------------------------------------
CactKernel build complete!
  Version:  0.9.0
  Commit:   abc1234
  Built:    2026-04-25 12:00:00
  Kernel:   build/kernel.bin
  Image:    build/cact.iso
--------------------------------------------------
```

**Compiler flags:**
```makefile
CFLAGS = -m32 -ffreestanding -fno-pie -fno-stack-protector -nostdlib -Wall
# Version metadata embedded at build time:
VERSION_DEFS = -DCACT_VERSION=0.9.0 -DCACT_COMMIT_HASH=$(git rev-parse --short HEAD)
```

**Linking:**
```makefile
LDFLAGS = -m elf_i386 -T linker.ld -z noexecstack
# Final link: C objects + liblux_mm.a (Rust PMM/VMM) + libsched.a (Rust MLFQ)
```

---

## 📂 Structure

```
CactKernel-x86_32/
├── Lux/
│   ├── kernel/
│   │   ├── core/           kernel.c, kernel.h, klib.c, syscall.c, multiboot2.c
│   │   ├── memory/         rust_mm/ (PMM, VMM, mmap, swap, slab, shm — Rust)
│   │   ├── proc/           task.c, task.asm, sched/ (MLFQ — Rust)
│   │   ├── sync/           spinlock, IRQ spinlock, mutex, semaphore (Rust)
│   │   ├── elf/            elf_loader.c, dynlink/dynlink.c
│   │   └── gdt/ idt/ shell/
│   ├── drivers/
│   │   ├── block/          AHCI, NVMe, blkdev, buf, pagecache
│   │   ├── input/          ps_2/ keyboard + mouse, io.asm
│   │   ├── network/        virtio_net/
│   │   ├── pci/            pci, enum, driver, loader
│   │   ├── usb/            xHCI (~32KiB), hid, hub
│   │   └── video/          fb/ (32bpp framebuffer), font/ (8×8 bitmap)
│   ├── fs/
│   │   ├── vfs/            vfs.c, devfs, procfs, mntfs, etcfs, tmpfs, binfs
│   │   ├── ext4/           ~40KiB, read/write
│   │   └── btrfs/ exFAT/ ramfs/  ← stubs (7 bytes each)
│   ├── net/                arp, ethernet, ip, icmp, protocols/udp+tcp, socket
│   └── pipe/               pipe.c
├── Makefile                → build/cact.iso
├── VERSION                 0.9.0
├── linker.ld               kernel link script (elf_i386)
└── grub.cfg                multiboot2 boot entry
```

---

## 🚀 Boot Sequence

Full initialization order in `kernel_setup_hardware()` + `init()`:

| # | Component | Notes |
|---|-----------|-------|
| 01 | **Multiboot2 parse** | magic check (0x36D76289), framebuffer tag, memory map |
| 02 | **GDT + Memory Manager** | init_gdt(), init_memory_manager(), init_heap() |
| 03 | **Framebuffer identity-map** | vmm_map() for each 4K FB page, then init_paging() |
| 04 | **Slab + Page Fault** | slab_init(); page_fault_init() — 7-scenario handler |
| 05 | **PIC + IDT** | init_pic() — 8259A; init_idt() — 32 CPU ISRs |
| 06 | **Framebuffer ready** | 32bpp, 8×8 font ×2 scale, 16-color palette |
| 07 | **PS/2 + I/O probe** | ps2_keyboard_init(), ps2_mouse_init(), port 0x64 probe |
| 08 | **PCI scan + enumerate** | search_pci(), pci_enumerate() — binds AHCI/NVMe/virtio-net |
| 09 | **USB xHCI stack** | usb_init() — xHCI host controller, HID, Hub |
| 10 | **Block layer + Swap** | blkdev_init(), pc_init() (page cache), swap_init() |
| 11 | **VFS + Filesystems** | vfs_init(), mntfs_init() — ext4/devfs/procfs/mntfs/etcfs |
| 12 | **Network** | net_init() — ethernet, ARP, IPv4, ICMP, UDP, TCP |
| 13 | **Tasks + Scheduler** | task_init(), init_scheduler() — MLFQ 4-level Rust |
| 14 | **PIT Timer 100 Hz** | init_timer(100) — divisor=11931 → IRQ0 active |
| 15 | **Shell ready → STI** | system_ready=1; sti; terminal spawned; kernel idles on HLT |

**Boot prompt:**
```
        [  OK  ] EXT4 / devfs / procfs / mntfs mounted
        [  OK  ] PIT timer @ 100 Hz — IRQ0 active
        [  OK  ] shell commands registered

CactKernel 0.9.0
--------------------------
Kernel is ready. Starting terminal...

Cact Shell ready!
kernel@cact:~$
```

---

## 🧠 Memory Map

### Physical (rust_mm constants)

| Constant | Value | Notes |
|----------|-------|-------|
| `MEM_START` | `0x00100000` | Physical base (1 MB) |
| `MEM_SIZE` | 128 MB | Managed physical range |
| `TOTAL_PAGES` | 32 768 | MEM_SIZE / PAGE_SIZE |
| `HEAP_START` | MEM_START + bitmap | Kernel heap base |
| `HEAP_SIZE` | 16 MB | Kernel heap ceiling |
| `HEAP_MAGIC` | `0xDEADBEEF` | Block integrity check |
| `SWAP_MAX_SLOTS` | 65 536 | Swap bitmap slots |
| `SLAB_MIN/MAX` | 8–2048 B | Slab object sizes |

### User Address Space

```
0xC0000000  ┌──────────────────────────┐  Kernel space (ring 0 only)
0xBFFFE000  ├──────────────────────────┤  sigreturn trampoline (int 0x80)
0xBF000000  ├──────────────────────────┤  User stack top (grows ↓)
            │         USER STACK        │  LIMIT=0xBF000000 → TOP=0xC0000000
0xA0000000  ├──────────────────────────┤  Shared memory (SHM)
            │      SHM_VA_BASE → 0xB0000000  (32 segs × 64 pages)
0x40000000  ├──────────────────────────┤  mmap region (256 slots max)
            │    MMAP_BASE → 0xBF000000  │
0x40000000  ├──────────────────────────┤  User heap (brk)
            │  USER_HEAP_START → 0x80000000
0x08048000  ├──────────────────────────┤  ELF text + data (PT_LOAD)
0x00000000  └──────────────────────────┘  NULL guard (unmapped)
```

### PTE flags (Rust ffi.rs)

```rust
PAGE_PRESENT  = 0x001   // mapped
PAGE_RW       = 0x002   // writable
PAGE_USER     = 0x004   // ring 3 accessible
PAGE_COW      = 0x200   // copy-on-write
PAGE_DEMAND   = 0x400   // demand paging
PAGE_ZERO     = 0x800   // zero page
PAGE_SWAPPED  = 0x008   // swapped out (PRESENT=0)
```

---

## ⏱️ Scheduler — MLFQ (Rust)

| Level | Name | Quantum | Notes |
|-------|------|---------|-------|
| 0 | Real-Time | 5 ticks | Highest priority |
| 1 | Interactive | 1 tick | Destination of priority boost |
| 2 | Normal | 2 ticks | Default for new tasks |
| 3 | Background | 4 ticks | Long-running, CPU-bound |

- **Priority boost** каждые 50 тиков: все задачи ≥ Normal → Interactive (anti-starvation)
- **Voluntary block bonus**: блокировка до половины кванта → повышение на уровень
- **Sleep queue**: `wake_expired_sleepers()` на каждом тике
- **Timer wheel**: `timer_wheel_tick()` + `check_alarm_timers()` на каждом тике
- **Reentrance guard**: `SCHEDULE_IN_PROGRESS` атомарный флаг

**Task states:**
```c
TASK_READY    = 0   // in run queue
TASK_RUNNING  = 1   // on CPU
TASK_SLEEPING = 2   // waiting for sleep_until tick
TASK_ZOMBIE   = 3   // exit() called, awaiting waitpid()
TASK_WAITING  = 4   // blocked on I/O or mutex
```

---

## 💾 Drivers

| Category | Driver | Status |
|----------|--------|--------|
| **Block** | AHCI (SATA) | ✅ Active |
| **Block** | NVMe | ✅ Active |
| **Block** | blkdev abstraction | ✅ Active |
| **Block** | buffer cache (buf) | ✅ Active |
| **Block** | page cache | ✅ Active |
| **USB** | xHCI 3.0 (~32 KiB) | ✅ Active |
| **USB** | USB HID (kbd/mouse) | ✅ Active |
| **USB** | USB Hub (cascading) | ✅ Active |
| **Input** | PS/2 Keyboard | ✅ Active |
| **Input** | PS/2 Mouse | ✅ Active |
| **Video** | Framebuffer 32bpp | ✅ Active |
| **Video** | 8×8 bitmap font ×2 | ✅ Active |
| **PCI** | Bus scan + enum + driver binding | ✅ Active |
| **Network** | virtio-net (QEMU/KVM) | ✅ Active |

---

## 📁 Filesystems

| FS | Status | Notes |
|----|--------|-------|
| **ext4** | ✅ Active | ~40 KiB, read/write, inode ops |
| **VFS** | ✅ Active | 32-slot mount table, symlinks, ELOOP, rwx permissions |
| **devfs** | ✅ Active | Devices as VFS nodes |
| **procfs** | ✅ Active | /proc/cmd, register_cmd, meminfo |
| **mntfs** | ✅ Active | mount/umount/list, auto-mount at boot |
| **etcfs** | ✅ Active | /etc/passwd-like, uid↔name mapping |
| **tmpfs** | ✅ Active | In-memory FS |
| **binfs** | ✅ Active | Binary/exec namespace |
| **pipes** | ✅ Active | pipe_create, fd table integration |
| **btrfs** | ⬜ Stub | 7 bytes placeholder |
| **exFAT** | ⬜ Stub | 7 bytes placeholder |
| **ramfs** | ⬜ Stub | 7 bytes placeholder |

---

## 🌐 Network Stack

```
TCP state machine:
CLOSED → LISTEN → SYN_SENT → SYN_RECEIVED
       → ESTABLISHED
       → FIN_WAIT_1 → FIN_WAIT_2 → TIME_WAIT
       → CLOSE_WAIT → LAST_ACK → CLOSED
```

| Layer | Features |
|-------|----------|
| **skb** | alloc/free/push/put — kernel packet buffer |
| **Ethernet** | ethernet_input demux by EtherType |
| **ARP** | cache, request/reply |
| **IPv4** | ip_output, pseudo-checksum, inet_checksum |
| **ICMP** | echo request (ping) |
| **UDP** | udp_output, sock alloc/recv/free |
| **TCP** | Full state machine, RST, accept(), RX ring buffer, callbacks |
| **Sockets** | 16-slot kernel table, VFS integration (read/write/close) |
| **knetd** | Kernel-space daemon on semaphore, net_poll() |

> ⚠️ Статический IP (`MY_IP`). DHCP и DNS не реализованы в pre-1.0.

---

## 💥 Kernel Panic

При исключении в **ring 0** — полный дамп регистров и halt:
```
=== KERNEL PANIC ===
Exception: 14 (#PF)   Error code: 0x00000003
EIP: 0xC010A3F2   CS: 0x00000008
EAX: 0x00000000   EBX: 0xDEADBEEF   ECX: 0x00000001   EDX: 0x00000000
ESP: 0xC01FF9E0   EBP: 0xC01FFA10
System halted.
```

При исключении в **ring 3** — маппинг на POSIX-сигнал:

| Exception | Signal | Condition |
|-----------|--------|-----------|
| #DE (int 0) | `SIGFPE` | Divide by zero |
| #MF (int 16) | `SIGFPE` | x87 FPU error |
| #GP (int 13) | `SIGSEGV` | General protection fault |
| all others | `SIGKILL` | Unrecoverable crash |

---

## 📞 System Calls (73 total)

| Group | Syscalls |
|-------|----------|
| **Files** | `open` `read` `write` `close` `create` `delete` `lseek` `stat` `fstat` `getdents` `rename` `mkdir` `rmdir` `fcntl` `ioctl` `symlink` `readlink` `link` `unlink` |
| **Processes** | `fork` `exec` `exit` `kill` `signal` `sigaction` `sigreturn` `sigprocmask` `sigpending` `sigsuspend` `getpid` `getppid` `waitpid` `sleep` `brk` `alarm` `setitimer` |
| **Memory** | `mmap` `munmap` `mprotect` `shmget` `shmat` `shmdt` `shmctl` |
| **Network** | `socket` `bind` `connect` `listen` `accept` `send` `recv` `sendto` `recvfrom` `shutdown` `setsockopt` `getsockopt` `select` `poll` |
| **I/O** | `pipe` `dup2` `getcwd` `chdir` |
| **Users** | `getuid` `getgid` `setuid` `setgid` `geteuid` `getegid` `chmod` `chown` |
| **Time** | `gettimeofday` `clock_gettime` `nanosleep` |
| **Misc** | `print` |

---

## ⚖️ License

**GNU General Public License v3.0** — see [`LICENSE`](LICENSE)

---

**Developer:** [QwaYer](https://github.com/QwaYer) · **libc:** [CactLib-x86_32](https://github.com/QwaYer/CactLib-x86_32) · **OS:** [CactOS-x86_32](https://github.com/QwaYer/CactOS-x86_32)
