# 🌵 CactKernel/x86_32

<p align="center">
  <img src="https://img.shields.io/badge/version-1.0.0-green.svg?style=for-the-badge" alt="Version: 1.0.0">
  <img src="https://img.shields.io/badge/license-GPLv3-blue.svg?style=for-the-badge" alt="License: GPLv3">
  <img src="https://img.shields.io/badge/arch-i686-red.svg?style=for-the-badge" alt="Arch: i686">
  <img src="https://img.shields.io/badge/language-C%2FRust%2FASM-orange.svg?style=for-the-badge" alt="Language: C/Rust/ASM">
  <img src="https://img.shields.io/badge/boot-Multiboot2-purple.svg?style=for-the-badge" alt="Multiboot2">
  <img src="https://img.shields.io/badge/output-cact.iso-0369a1.svg?style=for-the-badge" alt="cact.iso">
  <img src="https://img.shields.io/badge/status-pre--1.0-yellow.svg?style=for-the-badge" alt="pre-1.0">
</p>

<p align="center">
  A <strong>hybrid monolithic kernel</strong> for <strong>i686</strong> (32-bit x86 protected mode).<br>
  Low-level code in <strong>C</strong> and <strong>NASM</strong>; the <strong>physical/virtual memory manager</strong>, <strong>MLFQ scheduler</strong>, <strong>synchronization primitives</strong>, and the <strong>TCP/UDP/DHCP/DNS stack (smoltcp)</strong> live in <strong>Rust</strong> crates <code>cact_mm</code>, <code>sched</code>, <code>sync</code>, and <code>cact_net</code>.
</p>

---

## 📊 Stats

| | |
|---|---|
| **Syscalls** | 95 — authoritative enum in [`syscalls.h`](Cact/kernel/core/syscalls/syscalls.h) (`SYSCALL_COUNT`) |
| **CPU ISRs** | 32 (IDT) + IRQ stubs via PIC |
| **PMM range** | Physical frames **0 … `0xE000_0000`** (RAM below the PCI/MMIO hole; actual RAM from Multiboot2 mmap) |
| **MAX_FD** | 256 file descriptors per task (`rust_mm` FFI) |
| **Kernel sockets** | `KSOCK_MAX` 16 VFS socket nodes; `TCP_MAX_SOCKETS` 8; `UDP_SOCK_MAX` 8 (see `rust_net` / `tcp.h`) |
| **xHCI** | ~32 KiB host stack (USB 3.x) |
| **Scheduler** | 4-level MLFQ (Rust) |
| **ext4 (in-tree)** | ~40 KiB — read/write, inode operations |

---

## 🔗 Ecosystem & full-disk workflow

CactKernel is one piece of a larger workspace. Typical pieces:

| Component | Role |
|-----------|------|
| **[CactLib-x86_32](https://github.com/QwaYer/CactLib-x86_32)** | Userspace **`libc.a`** / **`libc.so`**. Every `SYS_*` number must match the kernel’s [`syscalls.h`](Cact/kernel/core/syscalls/syscalls.h). After any syscall change: rebuild libc and **re-link all ELFs** (init, shell, demos). |
| **[LocalRepoCactOS](../LocalRepoCactOS)** | Builds relocatable **`.cctk`** PCI modules, stages ELF binaries under **`lib/bin/`**, and packs a single GRUB module **`cctkfs.img`**. GRUB loads it as `module2 /boot/cctkfs.img cctkfs` (see [`grub.cfg`](grub.cfg)). |
| **[`build-cact-qemu.sh`](../build-cact-qemu.sh)** | One-shot: driver repos → **`cctkfs.img`** → [`build_disk.sh`](build_disk.sh) (empty **ext4** **`build/nvme.img`**, default 512 MiB) → **`make`** in this tree → **`build/cact.iso`**. |

**Why `cctkfs` exists:** the kernel copies the Multiboot2 “cctkfs” module into a large **`.bss`** staging buffer **before paging** (`pci_modblob_load`). At runtime, **binfs / sbinfs / libfs** overlay files from that archive on top of ext4 (e.g. **`/bin/init`**, **`libc.so`**, optional **`*.cctk`** drivers). PCI dynamic loading reads ET_REL blobs from the same archive.

From the workspace root (QEMU-oriented full rebuild):

```sh
./build-cact-qemu.sh
# Kernel only (expects ../LocalRepoCactOS/cctkfs.img already packed):
cd CactKernel-x86_32 && make -j"$(nproc)"
```

Hardware-focused ISO without relying on a root disk layout: `make GRUB_CFG=grub.cfg.ramroot`.

---

## 🔨 Building

**Recommended — full workspace**

Use sibling **[CactOS-x86_32](https://github.com/QwaYer/CactOS-x86_32)**: from the common parent directory run **`make`** or **`make -C CactOS-x86_32 iso`** — **CactOS** drives **CactLib**, user programs, **LocalRepo**, this kernel, and **CactBridge**.

**Standalone — this repository**

**Toolchain**

| Tool | Notes |
|------|-------|
| `gcc -m32` | Multilib on amd64 — e.g. `gcc-multilib` (Debian/Ubuntu) |
| `nasm` | Multiboot2 entry + interrupt stubs |
| `ld -m elf_i386` | GNU binutils |
| `cargo +nightly` | Builds `rust_mm`, `sched`, `cact_net` with **`-Z build-std=core,compiler_builtins`** and the **`i686-cact`** JSON target |
| `grub-mkrescue` + `xorriso` | Kernel **`Makefile`** can produce **`build/cact.iso`** |
| `qemu-img`, `mkfs.ext4`, `e2fsck` | For [`build_disk.sh`](build_disk.sh) |

**Common targets**

```sh
make -j"$(nproc)"     # kernel + default ISO (kernel-only multiboot layout)
make sched            # Rust scheduler crate only
make clean            # wipe build/ and Rust artifacts used by the Makefile
./build_disk.sh       # create empty ext4 nvme.img for ./run_qemu.sh
```

**ISO with bundled `cctkfs.img` (`iso-full`)**

```sh
make iso-full   # auto-detects ../LocalRepoCactOS
```

Override if needed: `make iso-full LOCAL_REPO=/path/to/LocalRepoCactOS`.

**QEMU:** set **`CACT_ISO`** to your **`cact.iso`**, or drop **`cact.iso`** into **`build/`**, then [`./run_qemu.sh`](./run_qemu.sh).

> 🧩 **`python3`** is only needed in **LocalRepoCactOS** to pack **`cctkfs.img`** — not for the default kernel **`make`**.

**Successful build footer** (version from [`VERSION`](VERSION), commit from `git`):

```
--------------------------------------------------
Cact kernel build complete!
  Version: 1.0.0
  Commit:  <short>
  Built:   <timestamp>
  Kernel:  build/kernel.bin
  Image:   build/cact.iso
--------------------------------------------------
```

**Version macros** (from [`Makefile`](Makefile)):

```makefile
VERSION_DEFS = -DCACT_VERSION=$(CACT_VERSION) \
               -DCACT_COMMIT_HASH=$(CACT_COMMIT) \
               -DCACT_BUILD_TIME="$(CACT_BUILD_TIME)"
```

**Final link** (simplified): all C objects + **`libcact_mm.a`** (PMM/VMM/brk/mmap) + **`libsched.a`** (MLFQ) + **`libcact_net.a`** (smoltcp, virtio PHY shim, DHCP, ICMP, DNS resolver, TCP/UDP socket glue). Link script: [`linker.ld`](linker.ld) with **`-z noexecstack`**.

Optional: `KERN_DEBUG=1 make` for richer symbols; QEMU GDB: see [`run_qemu.sh`](run_qemu.sh).

---

## 📂 Repository layout

```
CactKernel-x86_32/
├── Cact/
│   ├── kernel/
│   │   ├── core/        kernel entry, Multiboot2, syscall dispatch, IRQ, klib
│   │   ├── memory/      rust_mm/ — PMM, VMM, page faults, mmap, swap, slab, SHM
│   │   ├── proc/        task_struct, context switch, sched/ (Rust MLFQ)
│   │   ├── sync/        locks, semaphores (Rust + C FFI)
│   │   ├── elf/         static ELF loader, dynlink/ for relocatable objects
│   │   ├── gdt/ idt/
│   │   └── net/         legacy C path (ARP, IP, …) + rust_net/ (smoltcp)
│   ├── drivers/
│   │   ├── block/       blkdev, page cache
│   │   ├── input/       PS/2 keyboard & mouse
│   │   ├── network/     virtio-net (in-tree NIC for QEMU)
│   │   ├── pci/         enumerator, GDD, ELF module loader, cctkfs staging
│   │   ├── usb/         xHCI + HID + hub
│   │   └── video/       framebuffer console, font, MTRR WC + shadow blit
│   ├── fs/
│   │   ├── vfs/         core VFS, devfs, procfs, mntfs, etcfs, tmpfs,
│   │   │                binfs, sbinfs, libfs, varfs
│   │   ├── ext4/
│   │   └── btrfs/ exFAT/ ramfs/   ← tiny stubs
│   └── pipe/
├── Makefile
├── VERSION
├── linker.ld
├── grub.cfg              # multiboot2 kernel + cctkfs module
├── grub.cfg.ramroot      # RAM-first userland variant
├── build_disk.sh         # raw ext4 image for QEMU AHCI/NVMe
└── run_qemu.sh           # launches QEMU; runs build_disk.sh if nvme.img missing
```

---

## 🚀 Boot sequence

Boot is split into **three phases**: early `init()` (identity map, no user IRQs yet), **`kernel_setup_hardware()`** (bring up devices and subsystems), then a **bootstrap kernel thread** that can sleep on semaphores while mounting storage.

### Phase A — early `init()` (still single stack, interrupts globally masked)

| Step | What happens |
|------|----------------|
| 1 | **Multiboot2** parse — memory map, framebuffer tag, modules |
| 2 | **cctkfs staging** — `pci_modblob_load()` copies the GRUB “cctkfs” module from its physical address into kernel **`.bss`** before paging is enabled |
| 3 | **Framebuffer** — `fb_init()`; if no FB tag / zero size → halt (blind) |
| 4 | Magic check (`0x36D76289`) |
| 5 | **`kernel_setup_hardware()`** — see Phase B |
| 6 | **`create_task(kernel_bootstrap_main)`** — deferred work that needs the scheduler |
| 7 | **`sti`** — boot thread becomes the **idle** task (HLT loop); timer IRQ drives preemption |

### Phase B — `kernel_setup_hardware()`

Order matters (e.g. **blkdev** before PCI so AHCI/NVMe can register; **PIT** before PCI enumeration for GDD timeouts while IRQs are still masked globally).

| # | Subsystem |
|---|-----------|
| 1 | **GDT** → **PMM** (from MB2 mmap) → **VMM** → **kmalloc heap** → **paging on** |
| 2 | **Slab allocator** + **page fault** handler (COW, demand zero, swap markers) |
| 3 | **PIC** + **IDT** + **COM1 serial** (mirrors part of `kprint` / `klog` to host) |
| 4 | **Linear framebuffer** console, **MTRR** write-combining for VRAM, optional **shadow buffer** (WC + batched blit) |
| 5 | **PS/2** keyboard & mouse; optional warnings if I/O port `0x64` reads `0xFF` or CMOS memory size looks wrong |
| 6 | **PIT @ 100 Hz** — timer ticks before PCI scan (driver prompts) |
| 7 | **`blkdev_init`** → **PCI bus scan** + **enumeration** → **`usb_init`** (xHCI) |
| 8 | **Page cache** + **swap** (optional swap partition; failure logs a warning) |
| 9 | **`vfs_init`** + **`net_init`** (Rust `stack_init`, **`net_poll_task`** thread on semaphore + `net_poll` / `stack_poll`) |
| 10 | **`task_init`** + **`init_scheduler`** (Rust MLFQ) |

### Phase C — `kernel_bootstrap_main` (first real task)

| # | Action |
|---|--------|
| 1 | **`pci_driver_probe_deferred_all()`** — attach PCI drivers that were not safe at pure boot time |
| 2 | **`mntfs_init`** — parse mount table, **mount ext4** on NVMe/AHCI (may **`sema_down`** waiting for IRQ completions — **illegal** from the raw boot stack, hence this thread) |
| 3 | **`create_elf_task("bin/init")`** — first userspace process; binary resolved through **binfs** (ext4 `/bin` + **cctkfs** overlay) |

**Typical serial / FB banner:**

```
Cact Kernel 1.0.0
--------------------------
[VER] commit=…  built=…
Kernel is ready. Launching init…
```

---

## 🧠 Memory map (`rust_mm`)

The PMM treats **all 4 GiB of physical address space** below the **PCI hole** as frame-indexable. Frames inside the **low 32 MiB** reservation (BIOS, kernel image, static page tables) are permanently marked used. Usable RAM above that comes from the **Multiboot2 memory map**; the static upper bound for bitmap sizing is **`PCI_HOLE_START` (`0xE000_0000`)** — about **3584 MiB** of addressable frames.

| Symbol | Value | Meaning |
|--------|-------|---------|
| `MEM_START` | `0x00100000` | Conventional kernel load floor |
| `PCI_HOLE_START` | `0xE0000000` | First address **not** handed out by the PMM (MMIO / PCI) |
| `MEM_SIZE` | `PCI_HOLE_START` | Span covered by the frame bitmap |
| `TOTAL_PAGES` | `MEM_SIZE / 4096` | e.g. 917 504 pages |
| `BITMAP_SIZE` | `TOTAL_PAGES / 8` | Bitmap byte count (~112 KiB worst case) |
| `RESERVED_END` | `0x02000000` (32 MiB) | Low memory never given to `kalloc`/user |
| `HEAP_START` / `HEAP_SIZE` | `0x02000000` / 16 MiB | Kernel heap window |
| `HEAP_MAGIC` | `0xDEADBEEF` | Heap block canary |
| `SWAP_MAX_SLOTS` | 65536 | Swap bitmap |
| `SLAB_MIN/MAX` | 8 … 2048 B | Slab object sizes |

### User virtual layout (reference)

```
0xC0000000  ┌──────────────────────────┐  Kernel-only (ring 0)
0xBF000000  ├──────────────────────────┤  User stack floor
            │       user stack ↓        │
0xBEFFF000  ├──────────────────────────┤  Per-process sigreturn trampoline page (`int 0x80` stub)
0xB0000000  ├──────────────────────────┤  SHM ceiling (`SHM_VA_LIMIT`)
0xA0000000  ├──────────────────────────┤  SHM base (`SHM_VA_BASE`)
0x80000000  ├──────────────────────────┤  User heap ceiling (`USER_HEAP_LIMIT`)
0x40000000  ├──────────────────────────┤  mmap + brk region (`MMAP_BASE` … `MMAP_LIMIT`, up to 256 regions)
0x08048000  ├──────────────────────────┤  Typical ELF `PT_LOAD` base
0x00000000  └──────────────────────────┘  NULL / guard
```

### Page / directory bits (excerpt)

| Flag | Hex | Role |
|------|-----|------|
| `PAGE_PRESENT` | `0x001` | Mapped |
| `PAGE_RW` | `0x002` | Writable |
| `PAGE_USER` | `0x004` | User accessible |
| `PAGE_PWT` / `PAGE_SWAPPED` | `0x008` | Write-through in PTE; when **PRESENT=0**, software marks **swapped** pages |
| `PAGE_PCD` | `0x010` | Cache disable — MMIO |
| `PAGE_COW` | `0x200` | Copy-on-write |
| `PAGE_DEMAND` | `0x400` | Demand-filled / zero-on-first-touch |
| `PAGE_ZERO` | `0x800` | Zero-fill on demand |
| `PDE_PRIVATE` | `0x200` in **PDE** | CPU-ignored tag: “this page table is per-process” for fork/COW teardown |

---

## ⏱️ Scheduler — MLFQ (Rust)

| Level | Name | Quantum | Typical use |
|-------|------|---------|--------------|
| 0 | Real-time | 5 ticks | Highest priority work |
| 1 | Interactive | 1 tick | Boost target (latency-sensitive) |
| 2 | Normal | 2 ticks | Default for new tasks |
| 3 | Background | 4 ticks | CPU-bound batch work |

- **Anti-starvation boost** every **50 ticks**: tasks at **Normal** or lower move toward **Interactive**.
- **Voluntary block bonus**: if a task blocks for more than half its quantum, it may gain a priority level when it wakes.
- **Sleep queue** + **alarms** / **`setitimer`** hooks run on each timer tick.
- **Re-entrancy guard** (`SCHEDULE_IN_PROGRESS`) prevents nested scheduler entry.

**Task states:** `TASK_READY`, `TASK_RUNNING`, `TASK_SLEEPING`, `TASK_ZOMBIE`, `TASK_WAITING`.

---

## 💾 Drivers

| Area | Components | Notes |
|------|------------|-------|
| **Block** | AHCI, NVMe, blkdev, page cache | In-tree drivers; additional storage stacks can ship as **`.cctk`** in **`cctkfs.img`** |
| **USB** | xHCI, HID, hub | ~32 KiB host code path |
| **Input** | PS/2 keyboard & mouse | |
| **Video** | Linear FB 32 bpp, 8×8 font (×2 scale), MTRR WC + shadow | Scroll coalesces writes then blits |
| **PCI** | Config scan, driver table, **GDD** (generic device declarations), **modblob** loader | Loads ET_REL modules from **cctkfs** or path |
| **Network** | **virtio-net** | Default NIC under QEMU; other NICs often packaged as **`.cctk`** (e.g. Marvell **Yukon** in sibling repos) |

Extra PCI drivers live in **`*-for-Cact`** repositories; **`make -C CactOS-x86_32`** (workspace integrator) installs them into **`LocalRepoCactOS/lib/`** and packs **`cctkfs.img`**.

---

## 📁 Filesystems

| FS | Status | Notes |
|----|--------|-------|
| **ext4** | Active | Small in-kernel subset — read/write, inodes |
| **VFS** | Active | Up to **`VFS_MOUNT_MAX` (32)** simultaneous mount points, symlink pool with **ELOOP** detection, `rwx` permission bits |
| **devfs** | Active | Device nodes as VFS files |
| **procfs** | Active | e.g. `/proc/cmd`, `meminfo`, module listings |
| **mntfs** | Active | User-visible mount table + auto-mount policy at boot |
| **etcfs** | Active | passwd-like uid ↔ name mapping |
| **tmpfs** | Active | RAM-backed files |
| **binfs** | Active | **`/bin`** with **cctkfs** overlay (user ELF) |
| **sbinfs** | Active | **`/sbin`** + cctkfs |
| **libfs** | Active | **`/lib`** + **`libc.so`** from cctkfs |
| **varfs** | Active | **`/var`** layout |
| **pipes** | Active | `pipe()` integrated with the fd table |
| **btrfs / exFAT / ramfs** | Stub | Placeholder headers only |

---

## 🌐 Network stack

Logical TCP states (C metadata / VFS view; ingress TCP is handled by **smoltcp**):

```
CLOSED → LISTEN → SYN_SENT → SYN_RECEIVED
       → ESTABLISHED
       → FIN_WAIT_1 → FIN_WAIT_2 → TIME_WAIT
       → CLOSE_WAIT → LAST_ACK → CLOSED
```

The legacy **C** path still owns **Ethernet demux**, **ARP**, parts of **IPv4/ICMP**, and **`skb`** lifetime. **TCP/UDP sockets** for syscalls are backed by **smoltcp** inside **`cact_net`**: `stack_poll()` drives the iface, **DHCPv4** updates runtime IPv4 + DNS server IP, **`SYS_DNS_RESOLVE`** performs a blocking **A-record** query over UDP/53, and **`SYS_PING_ECHO`** sends ICMP echo requests.

| Layer | Responsibility |
|-------|----------------|
| **skb** | Kernel packet buffer alloc/push/pull |
| **Ethernet / ARP / IPv4 / ICMP** | Mostly C; ICMP echo path bridges into Rust |
| **TCP / UDP** | **smoltcp** sockets + C-side `ksock` / `tcp_socket_t` metadata |
| **Sockets / VFS** | Up to **16** kernel socket nodes integrated with `read`/`write`/`close` |
| **net_poll_task** | Dedicated kernel thread: sleeps on a semaphore, wakes on NIC RX, calls **`net_poll` → `stack_poll()`** |

**Limits (non-exhaustive):** no **IPv6**, no **TLS** inside the kernel; default NIC is **virtio-net** in QEMU; **`send`/`recv` flags** may be ignored in libc; DNS resolver is **A-record only** and needs a configured DNS IP (from DHCP or **`SYS_NETCFG_SET`**).

---

## 💥 Kernel panic & ring-3 faults

**Ring 0** — full register dump, message, **`cli; hlt`**:

```
=== KERNEL PANIC ===
Exception: 14 (#PF)   Error code: 0x00000003
EIP: 0xC010A3F2   CS: 0x00000008
EAX: 0x00000000   EBX: 0xDEADBEEF   ECX: 0x00000001   EDX: 0x00000000
ESP: 0xC01FF9E0   EBP: 0xC01FFA10
System halted.
```

**Ring 3** — some CPU exceptions are translated into Unix-like **signals** for the faulting task:

| Exception | Signal | Typical cause |
|-----------|--------|----------------|
| #DE (vector 0) | `SIGFPE` | Integer divide by zero |
| #MF (vector 16) | `SIGFPE` | x87 FPU fault |
| #GP (vector 13) | `SIGSEGV` | General protection fault |
| *others* | `SIGKILL` | Unmapped / unsupported fault path |

---

## 📞 System calls (95 total)

Authoritative list: [`Cact/kernel/core/syscalls/syscalls.h`](Cact/kernel/core/syscalls/syscalls.h) — must stay byte-for-byte in sync with **[CactLib `syscall.h`](https://github.com/QwaYer/CactLib-x86_32/blob/main/include/syscall.h)**.

Many syscalls take a **`struct syscall_frame*`** (full register snapshot) in the dispatcher — see [`mod.c`](Cact/kernel/core/syscalls/mod.c) `_needs_frame()`.

| Group | Calls |
|-------|-------|
| **Debug** | `print` |
| **Process** | `getpid` `getppid` `fork` `exec` `exit` `waitpid` `sleep` |
| **Session** | `setsid` `setpgid` `getpgid` `getpgrp` |
| **Signals** | `kill` `signal` `sigaction` `sigprocmask` `sigreturn` `sigpending` `sigsuspend` `alarm` `setitimer` |
| **FD / IO** | `open` `read` `write` `close` `lseek` `ioctl` `fcntl` `dup` `dup2` `pipe` `select` `poll` |
| **File metadata** | `stat` `fstat` `access` `chmod` `chown` `umask` `truncate` `ftruncate` `sync` `fsync` `mknod` |
| **Paths** | `create` `mkdir` `rmdir` `delete` `unlink` `rename` `link` `symlink` `readlink` `getdents` `chdir` `getcwd` `chroot` |
| **System** | `mount` `umount` `reboot` `uname` |
| **Memory** | `brk` `mmap` `munmap` `mprotect` |
| **SHM** | `shmget` `shmat` `shmdt` `shmctl` |
| **Time** | `gettimeofday` `clock_gettime` `nanosleep` |
| **Users** | `getuid` `getgid` `geteuid` `getegid` `setuid` `setgid` |
| **Network** | `socket` `bind` `connect` `listen` `accept` `send` `recv` `sendto` `recvfrom` `shutdown` `setsockopt` `getsockopt` `select` `poll` plus **`SYS_PING_ECHO` (90)**, **`SYS_NETCFG_SET` (91)**, **`SYS_DNS_RESOLVE` (94)** — see libc **`dns_resolve()`** |
| **Kernel modules** | `module_load` (92) `module_unload` (93) |

---

## ⚖️ License

**GNU General Public License v3.0** — see [`LICENSE`](LICENSE).

---

<p align="center">
  <strong>Developer:</strong> <a href="https://github.com/QwaYer">QwaYer</a>
  &nbsp;·&nbsp; <strong>libc:</strong> <a href="https://github.com/QwaYer/CactLib-x86_32">CactLib-x86_32</a>
  &nbsp;·&nbsp; <strong>OS:</strong> <a href="https://github.com/QwaYer/CactOS-x86_32">CactOS-x86_32</a>
</p>
