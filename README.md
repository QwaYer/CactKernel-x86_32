# 🌵 CactKernel/x86_32

<p align="center">
  <img src="https://img.shields.io/badge/version-2.0.0-green.svg?style=for-the-badge" alt="Version: 2.0.0">
  <img src="https://img.shields.io/badge/license-GPLv3-blue.svg?style=for-the-badge" alt="License: GPLv3">
  <img src="https://img.shields.io/badge/arch-i686-red.svg?style=for-the-badge" alt="Arch: i686">
  <img src="https://img.shields.io/badge/language-C%2FRust%2FASM-orange.svg?style=for-the-badge" alt="Language: C/Rust/ASM">
  <img src="https://img.shields.io/badge/boot-Multiboot2-purple.svg?style=for-the-badge" alt="Multiboot2">
  <img src="https://img.shields.io/badge/output-cact.iso-0369a1.svg?style=for-the-badge" alt="cact.iso">
  <img src="https://img.shields.io/badge/status-2.0.0-yellow.svg?style=for-the-badge" alt="2.0.0">
</p>

<p align="center">
  A <strong>hybrid monolithic kernel</strong> for <strong>i686</strong> (32-bit x86 protected mode).<br>
  Low-level code in <strong>C</strong> and <strong>NASM</strong>; the <strong>physical/virtual memory manager</strong>, <strong>MLFQ scheduler</strong>, <strong>synchronization primitives</strong>, <strong>TLS 1.3 (Rustls)</strong>, <strong>HMAC-SHA256 module signing</strong>, and the <strong>TCP/UDP/DHCP/DNS stack (smoltcp)</strong> live in <strong>Rust</strong> crates <code>cact_mm</code>, <code>sched</code>, <code>sync</code>, <code>rustls</code>, <code>cact_crypto</code>, and <code>cact_net</code>.
</p>

---

## 📊 Stats

| | |
|---|---|
| **Syscalls** | 15 core traps — authoritative enum in [`syscalls.h`](Cact/kernel/core/syscall/syscalls.h) (`SYSCALL_COUNT`); everything else via VFS-node ioctls ([`ioctl_abi.h`](Cact/kernel/core/syscall/ioctl_abi.h)) |
| **CPU ISRs** | 32 (IDT) + IRQ stubs via I/O APIC |
| **PMM range** | Physical frames **0 … `0xC000_0000`** (3 GiB; PCI hole lowered from 0xE0000000) |
| **MAX_FD** | 256 file descriptors per task (`rust_mm` FFI) |
| **Kernel sockets** | `KSOCK_MAX` 16 VFS socket nodes; `TCP_MAX_SOCKETS` 8; `UDP_SOCK_MAX` 8 (see `rust_net` / `tcp.h`) |
| **xHCI** | ~32 KiB host stack (USB 3.x) |
| **Scheduler** | 4-level MLFQ (Rust) |
| **ext4 (in-tree)** | ~40 KiB — read/write, inode operations |
| **TLS 1.3** | Rustls-based, in-kernel |
| **Task struct** | 48 bytes (optimised from 600 bytes) |

---

## 🔗 Ecosystem & full-disk workflow

CactKernel is one piece of a larger workspace. Typical pieces:

| Component | Role |
|-----------|------|
| **[CactLib-x86_32](https://github.com/QwaYer/CactLib-x86_32)** | Userspace **`libc.a`** / **`libc.so`**. Every `SYS_*` number must match the kernel’s [`syscalls.h`](Cact/kernel/core/syscall/syscalls.h) and `ioctl_abi.h`. After any syscall change: rebuild libc and **re-link all ELFs** (init, shell, demos). |
| **[LocalRepoCactOS](../LocalRepoCactOS)** | Builds relocatable **`.cctk`** PCI modules, stages ELF binaries under **`lib/bin/`**, and packs a single GRUB module **`cctkfs.img`**. GRUB loads it as `module2 /boot/cctkfs.img cctkfs` (see [`grub.cfg`](grub.cfg)). |
| **[`build-cact-qemu.sh`](../build-cact-qemu.sh)** | One-shot: driver repos → **`cctkfs.img`** → [`build_disk.sh`](build_disk.sh) (empty **ext4** **`build/nvme.img`**, default 512 MiB) → **`make`** in this tree → **`build/cact.iso`**. |

**Why `cctkfs` exists:** the kernel copies the Multiboot2 "cctkfs" module into a large **`.bss`** staging buffer **before paging** (`initfs_modblob_load`). At runtime, **binfs / sbinfs / libfs** overlay files from that archive on top of ext4 (e.g. **`/bin/init`**, **`libc.so`**, optional **`*.cctk`** drivers). PCI dynamic loading reads ET_REL blobs from the same archive. All modules are verified with **HMAC-SHA256** against the kernel's embedded static key before loading.

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
| `cargo +nightly` | Builds `rust_mm`, `sched`, `cact_net`, `rustls` with **`-Z build-std=core,compiler_builtins`** and the **`i686-cact`** JSON target |
| `grub-mkrescue` + `xorriso` | Kernel **`Makefile`** can produce **`build/cact.iso`** |
| `qemu-img`, `mkfs.ext4`, `e2fsck` | For [`build_disk.sh`](build_disk.sh) |

**Common targets**

```sh
make -j"$(nproc)"     # kernel + default ISO (kernel-only GRUB, grub.cfg.kernelonly)
make sched            # Rust scheduler crate only
make clean            # wipe build/ and Rust artifacts used by the Makefile
make iso-full         # full ISO with cctkfs.img (auto-detects ../LocalRepoCactOS)
./build_disk.sh       # create empty ext4 nvme.img for ./run_qemu.sh
```

**ISO with bundled `cctkfs.img` (`iso-full`)**

```sh
make iso-full   # auto-detects ../LocalRepoCactOS; signs cctkfs.img via tools/cact_sign_cctkfs.py
```

Override if needed: `make iso-full LOCAL_REPO=/path/to/LocalRepoCactOS`.

**QEMU:** set **`CACT_ISO`** to your **`cact.iso`**, or drop **`cact.iso`** into **`build/`**, then [`./run_qemu.sh`](./run_qemu.sh).

> 🧩 **`python3`** is only needed in **LocalRepoCactOS** to pack **`cctkfs.img`** and for **`tools/cact_sign_cctkfs.py`** — not for the default kernel **`make`** (`grub.cfg.kernelonly`).

**Successful build footer** (version from [`VERSION`](VERSION), commit from `git`):

```
--------------------------------------------------
Cact kernel build complete!
  Version: 2.0.0
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

**Final link** (simplified): all C objects + **`libcact_mm.a`** (PMM/VMM/brk/mmap) + **`libsched.a`** (MLFQ) + **`libcact_net.a`** (smoltcp, virtio PHY shim, DHCP, ICMP, DNS resolver, TCP/UDP socket glue) + **`librustls.a`** (TLS 1.3) + **`libcact_hmac_ffi.a`** (HMAC-SHA256 module signing). Link script: [`linker.ld`](linker.ld) with **`-z noexecstack`**.

Optional: `KERN_DEBUG=1 make` for richer symbols; QEMU GDB: see [`run_qemu.sh`](run_qemu.sh).

---

## 📂 Repository layout

```
CactKernel-x86_32/
├── Cact/
│   ├── kernel/
│   │   ├── core/        kernel entry, Multiboot2, syscall dispatch (sysenter), IRQ, klib
│   │   ├── memory/      rust_mm/ — PMM, VMM, page faults, mmap, swap, slab, SHM
│   │   ├── proc/        task_struct, context switch, sched/ (Rust MLFQ), lazy FPU, stack canary
│   │   ├── sync/        locks, semaphores (Rust + C FFI)
│   │   ├── elf/         static ELF loader, ksym/sym, dynlink/ for relocatable objects
│   │   ├── gdt/ idt/
│   ├── drivers/
│   │   ├── acpi/        ACPICA engine — AML interpreter, MADT/HPET/APIC tables
│   │   ├── block/       blkdev, page cache (increased constant limits)
│   │   ├── input/       USB HID only (PS/2 removed in 2.0)
│   │   ├── initfs/      cctkfs staging + module blob reader, HMAC signature verify
│   │   ├── pci/         enumerator, PCIe, ELF module loader,
│   │   │                HMAC-SHA256 module signature verification
│   │   ├── usb/         xhci + HID + hub
│   │   └── video/       framebuffer console, PSF2 font parser, PAT WC + shadow blit
│   ├── crypto/          Rustls — in-kernel TLS 1.3, HMAC-SHA256 signer, cact_shim
│   ├── fs/
│   │   ├── vfs/         core VFS, struct file, devfs, procfs, mntfs, etcfs, tmpfs,
│   │   │                binfs, sbinfs, libfs, usrfs, varfs
│   │   └── pipe/        kernel pipe implementation
│   └── net/             rust_net/ — pure-Rust stack (smoltcp: Ethernet/ARP/IP/ICMP/TCP/UDP/DHCP/DNS) + rustls TLS + HTTP(S) client, C FFI header
├── Makefile
├── VERSION
├── linker.ld
├── grub.cfg              # multiboot2 kernel + cctkfs module
├── grub.cfg.kernelonly   # kernel-only multiboot (default make target)
├── grub.cfg.ramroot      # RAM-first userland variant
├── build_disk.sh         # raw ext4 image for QEMU AHCI/NVMe
├── tools/                # utility scripts (gen_hmac_key.py, cact_sign_cctkfs.py, …)
└── run_qemu.sh           # launches QEMU; runs build_disk.sh if nvme.img missing
```

---

## 🚀 Boot sequence

Boot is split into **three phases**: early `init()` (identity map, no user IRQs yet), **`kernel_setup_hardware()`** (bring up devices and subsystems), then a **bootstrap kernel thread** that can sleep on semaphores while mounting storage.

### Phase A — early `init()` (still single stack, interrupts globally masked)

| Step | What happens |
|------|----------------|
| 1 | **Multiboot2** parse — memory map, framebuffer tag, modules |
| 2 | **cctkfs staging** — `initfs_modblob_load()` copies the GRUB "cctkfs" module from its physical address into kernel **`.bss`** before paging is enabled |
| 3 | **Framebuffer** — `fb_init()`; if no FB tag / zero size → halt (blind) |
| 4 | Magic check (`0x36D76289`) |
| 5 | **`kernel_setup_hardware()`** — see Phase B |
| 6 | **`create_task(kernel_bootstrap_main)`** — deferred work that needs the scheduler |
| 7 | **`sti`** — boot thread becomes the **idle** task (HLT loop); timer IRQ via HPET drives preemption |

### Phase B — `kernel_setup_hardware()`

Order matters (e.g. **blkdev** before PCI so AHCI/NVMe can register).

| # | Subsystem |
|---|-----------|
| 1 | **GDT** → **PMM** (from MB2 mmap) → **VMM** → **kmalloc heap** → **paging on** |
| 2 | **Slab allocator** + **page fault** handler (COW, demand zero, swap markers) |
| 3 | **I/O APIC** + **IDT** + **COM1 serial** (mirrors part of `kprint` / `klog` to host) |
| 4 | **ACPI** — parse RSDP, MADT, HPET tables via ACPICA |
| 5 | **HPET @ 100 Hz** — system timer |
| 6 | **Linear framebuffer** console, **PAT** write-combining for VRAM, optional **shadow buffer** (WB RAM + batched blit) |
| 7 | **USB** HID only (PS/2 removed in 2.0) |
| 8 | **`blkdev_init`** → **PCI bus scan** + **enumeration** (PCIe support) → **MSI-X allocation** → **`usb_init`** (xHCI) |
| 9 | **Page cache** + **swap** (optional swap partition; failure logs a warning) |
| 10 | **`vfs_init`** + **`net_init`** (Rust `stack_init`, **`net_poll_task`** thread on semaphore + `net_poll` / `stack_poll`) |
| 11 | **`task_init`** + **`init_scheduler`** (Rust MLFQ) |

### Phase C — `kernel_bootstrap_main` (first real task)

| # | Action |
|---|--------|
| 1 | **`pci_driver_probe_deferred_all()`** — attach PCI drivers that were not safe at pure boot time (all modules verified via HMAC-SHA256) |
| 2 | **`mntfs_init`** — parse mount table, **mount ext4** on NVMe/AHCI (may **`sema_down`** waiting for IRQ completions — **illegal** from the raw boot stack, hence this thread) |
| 3 | **`create_elf_task("bin/init")`** — first userspace process; binary resolved through **binfs** (ext4 `/bin` + **cctkfs** overlay) |

**Typical serial / FB banner:**

```
Cact Kernel 2.0.0
--------------------------
[VER] commit=…  built=…
Kernel is ready. Launching init…
```

---


---

## 🧠 Memory map (`rust_mm`)

The PMM treats **all 3 GiB of physical address space** below the **PCI hole** as frame-indexable. Frames inside the **low 32 MiB** reservation (BIOS, kernel image, static page tables) are permanently marked used. Usable RAM above that comes from the **Multiboot2 memory map**; the static upper bound for bitmap sizing is **`PCI_HOLE_START` (`0xC000_0000`)** — about **3 GiB** of addressable frames (reduced from 3.5 GiB in 1.x).

| Symbol | Value | Meaning |
|--------|-------|---------|
| `MEM_START` | `0x00100000` | Conventional kernel load floor |
| `PCI_HOLE_START` | `0xC0000000` | First address **not** handed out by the PMM (MMIO / PCI) |
| `MEM_SIZE` | `PCI_HOLE_START` | Span covered by the frame bitmap |
| `TOTAL_PAGES` | `MEM_SIZE / 4096` | e.g. 786 432 pages |
| `BITMAP_SIZE` | `TOTAL_PAGES / 8` | Bitmap byte count (~96 KiB worst case) |
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
0xBEFFF000  ├──────────────────────────┤  Per-process sigreturn trampoline page (sysenter stub)
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
| `PDE_PRIVATE` | `0x100` in **PDE** | CPU-ignored tag: "this page table is per-process" for fork/COW teardown |

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

## 💾 Drivers (in-tree)

| Area | Components | Notes |
|------|------------|-------|
| **Block** | AHCI, NVMe, blkdev, page cache | In-tree drivers; additional storage stacks can ship as **`.cctk`** in **`cctkfs.img`** |
| **USB** | xHCI, HID, hub | ~32 KiB host code path — PS/2 removed in 2.0 |
| **Input** | USB HID keyboard & mouse | |
| **Video** | Linear FB 32 bpp, PSF2 console font from cctkfs (`/lib/consolefont.psf`, ×2 scale), PAT WC + shadow | |
| **PCI** | Config scan, driver table, **modblob** loader, HMAC-SHA256 signature verification | Loads ET_REL modules from **cctkfs** or path (user-driven via kmod syscalls) |
| **Network** | **virtio-net** | Default NIC under QEMU; other NICs often packaged as **`.cctk`** (e.g. Marvell **Yukon** in sibling repos) |
| **ACPI** | ACPICA — RSDP, MADT, HPET, APIC table parsing | New in 2.0 |

All out-of-tree PCI drivers now use **MSI-X** instead of PIC IRQ lines. Extra PCI drivers live in **`*-for-Cact`** repositories; **`make -C CactOS-x86_32`** (workspace integrator) installs them into **`LocalRepoCactOS/lib/`** and packs **`cctkfs.img`**.

---

## 📁 Filesystems

| FS | Status | Notes |
|----|--------|-------|
| **ext4** | Active | Small in-kernel subset — read/write, inodes |
| **VFS** | Active | Up to **`VFS_MOUNT_MAX` (32)** simultaneous mount points, symlink pool with **ELOOP** detection, `rwx` permission bits, `struct file` with fd table |
| **devfs** | Active | Device nodes as VFS files |
| **procfs** | Active | e.g. `/proc/cmd`, `meminfo`, module listings |
| **mntfs** | Active | User-visible mount table + auto-mount policy at boot |
| **etcfs** | Active | passwd-like uid ↔ name mapping |
| **tmpfs** | Active | RAM-backed files |
| **binfs** | Active | **`/bin`** with **cctkfs** overlay (user ELF) |
| **sbinfs** | Active | **`/sbin`** + cctkfs |
| **libfs** | Active | **`/lib`** + **`libc.so`** from cctkfs |
| **usrfs** | Active | **`/usr`** layout |
| **varfs** | Active | **`/var`** layout |
| **pipes** | Active | `pipe()` integrated with the fd table |
| **btrfs / exFAT / ramfs** | Stub | Placeholder headers only |

---

## 🌐 Network stack

The **entire L3+ stack is pure Rust** (`cact_net`): Ethernet demux, ARP, IPv4, ICMP, TCP and UDP sockets, DHCPv4, and the DNS resolver all run on **smoltcp**; TLS 1.3 runs on the vendored **rustls** with the in-kernel `cact_crypto` provider. The C side is only a thin FFI layer (`rust_net_ffi.h` + syscall glue) — `net_shim.c` was removed in favour of Rust symbols (`net_receive_packet`, `net_driver_irq_wake`) that out-of-tree NIC modules resolve via the ksym table.

Logical TCP states (C metadata / VFS view; ingress TCP is handled by **smoltcp**):

```
CLOSED → LISTEN → SYN_SENT → SYN_RECEIVED
       → ESTABLISHED
       → FIN_WAIT_1 → FIN_WAIT_2 → TIME_WAIT
       → CLOSE_WAIT → LAST_ACK → CLOSED
```

`stack_poll()` drives the iface, **DHCPv4** updates runtime IPv4 + DNS server IP, **`SYS_DNS_RESOLVE`** performs a blocking **A-record** query over UDP/53, and **`SYS_PING_ECHO`** sends ICMP echo requests.

Full socket syscall API: `socket`, `bind`, `connect`, `listen`, `accept`, `send`, `recv`, `sendto`, `recvfrom`, `shutdown`, `setsockopt`, `getsockopt`.

| Layer | Responsibility |
|-------|----------------|
| **Ethernet / ARP / IPv4 / ICMP / TCP / UDP** | smoltcp (`cact_net`) |
| **Sockets / VFS** | Up to **16** kernel socket nodes integrated with `read`/`write`/`close`, `.poll` op |
| **net_poll_task** | Dedicated kernel thread: sleeps on a semaphore, wakes on NIC RX, calls **`net_poll` → `stack_poll()`** |
| **TLS 1.3** | In-kernel via rustls — `cact_tls_connect_ex` / `cact_tls_send` / `cact_tls_recv` / `cact_tls_close` |
| **HTTP/HTTPS** | `cact_http_request` / `cact_http_get` / `cact_http_post` — DNS → TCP → (TLS) → HTTP/1.1 fetch; `Content-Length`, chunked and read-to-close bodies |

**HTTP(S) FFI** (`Cact/net/rust_net_ffi.h`): the kernel can fetch pages without a userspace
process, e.g.

```c
cact_http_resp_t r;
static char buf[65536];
int rc = cact_http_get(&r, "https://example.com/", 0, 0, buf, sizeof(buf));
if (rc == 0) { /* r.status == 200, body at buf + r.body_off, r.body_len bytes */ }
```

HTTPS currently skips certificate chain verification (the `cact_crypto` provider has not
implemented certificate signature verification yet — RSA/ECDSA signature algorithms are
empty), so it works against arbitrary servers with a one-time warning. The verified
root-store path exists in `tls.rs` and is used by `cact_tls_connect`.

**Limits (non-exhaustive):** no **IPv6**; default NIC is **virtio-net** in QEMU; DNS resolver is **A-record only**; HTTP responses are buffered with a 1 MiB cap; no gzip decompression.

---

## 💥 Kernel panic & ring-3 faults

**Ring 0** — full register dump, call trace with symbol resolution (kernel + userspace ELF symtab), instruction bytes at EIP, **`cli; hlt`**:

```
=== KERNEL PANIC ===
Exception: 14 (#PF)   Error code: 0x00000003  [NP R S]
Fault address: 0xDEADBEEF
EIP: 0xC010A3F2   CS: 0x00000008   EFLAGS: 0x00010046
EAX: 0x00000000   EBX: 0xDEADBEEF   ECX: 0x00000001   EDX: 0x00000000
ESI: 0x00000000   EDI: 0x00000000   EBP: 0xC01FFA10   ESP: 0xC01FF9E0
 DS: 0x00000010   ES: 0x00000010   SS: 0x00000010
Call trace:
  [0] 0xC010A3F2 (kernel_function+0x1E)
  [1] 0xC01055A1 (init+0x41)
Code: 90 90 <8B> 45 08 89 04 24 E8 ...
System halted.
```

**Ring 3** — some CPU exceptions are translated into Unix-like **signals** for the faulting task:

| Exception | Signal | Typical cause |
|-----------|--------|----------------|
| #DE (vector 0) | `SIGFPE` | Integer divide by zero |
| #MF (vector 16) | `SIGFPE` | x87 FPU fault |
| #GP (vector 13) | `SIGSEGV` | General protection fault |
| #PF (vector 14) | `SIGSEGV` | Page fault (invalid address / protection) |
| *others* | `SIGKILL` | Unmapped / unsupported fault path |

The call trace walks the EBP chain and resolves addresses via the per-task ELF symbol table (both main binary and dynamically loaded shared objects), using `sym_resolve_addr()` from `Cact/kernel/elf/sym.c`.

---

## 📞 System calls (15 core traps)

The kernel only traps for 15 syscalls; everything else is a **VFS-node service**. A process `open()`s a node and issues `ioctl`/`read`/`write` on it.

Authoritative ABI headers: [`syscalls.h`](Cact/kernel/core/syscall/syscalls.h) (15 trap numbers) and [`ioctl_abi.h`](Cact/kernel/core/syscall/ioctl_abi.h) (every relay command/protocol struct) — both must stay byte-for-byte in sync with **[CactLib `syscall.h`](https://github.com/QwaYer/CactLib-x86_32/blob/main/include/syscall.h)**.

Syscall dispatch uses the **`sysenter`** CPU instruction (legacy `int 0x80` gate preserved as ring-3 fallback for `sigreturn` and CPUs without SEP — see [`idt.c`](Cact/kernel/idt/idt.c):102-104, [`cpudev.c`](Cact/kernel/cpudev/cpudev.c):208-210). Many syscalls take a **`struct syscall_frame*`** (full register snapshot) in the dispatcher — see [`mod.c`](Cact/kernel/core/syscall/mod.c) `_needs_frame()`.

| ABI surface | Service |
|-------|-------|
| **`SYS_*`** 0–14 | Core traps: `open` `close` `read` `write` `ioctl` `poll` `fork` `exec` `exit` `waitpid` `brk` `mmap` `munmap` `mprotect` `sigreturn` |
| **`CACT_FDCTL_*`** 0x3000 | ioctl on any fd — `dup` `dup2` `fcntl` `lseek` `fstat` `ftruncate` `getdents` `fsync` |
| **`CACT_DIRCTL_*`** 0x3100 | ioctl on a directory fd — `openat` `create` `mkdir` `rmdir` `unlink` `link` `symlink` `readlink` `rename` `stat` `access` `chmod` `chown` `truncate` `mknod` |
| **`CACT_PROCCTL_*`** 0x3200 | ioctl on `/proc/self\|pid/ctl` — `setsid` `setpgid` `setuid` `setgid` `umask` `chdir` `chroot` signals/itimers, `shmget` `shmat` `shmdt` `shmctl` |
| **`CACT_SOCKCTL_*`** 0x3300 | ioctl on a socket fd — `bind` `connect` `listen` `accept` `shutdown` `setsockopt` `getsockopt` `sendto` `recvfrom` (data path: `read`/`write`) |
| **`CACT_NETCTL_*`** 0x3400 | ioctl on `/dev/net` — `socket` creation, ping, DNS resolve, netcfg |
| **`CACT_SYSCTL_*`** 0x3500 | ioctl on `/dev/sys` (root) — `mount` `umount` `reboot`, kernel module load/unload |
| **`CACT_PIPECTL_*`** 0x3600 | ioctl on `/dev/pipe` — `pipe` creation |

Identity/process info: `/proc/self/info`; cwd: `/proc/self/cwd`; time: `/proc/time`; `uname`: `/proc/uname`.

---

## ⚖️ License

**GNU General Public License v3.0** — see [`LICENSE`](LICENSE).

---

<p align="center">
  <strong>Developer:</strong> <a href="https://github.com/QwaYer">QwaYer</a>
  &nbsp;·&nbsp; <strong>libc:</strong> <a href="https://github.com/QwaYer/CactLib-x86_32">CactLib-x86_32</a>
  &nbsp;·&nbsp; <strong>OS:</strong> <a href="https://github.com/QwaYer/CactOS-x86_32">CactOS-x86_32</a>
</p>
