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
  Low-level code in <strong>C</strong> and <strong>NASM</strong>; the <strong>physical/virtual memory manager</strong>, <strong>MLFQ scheduler</strong> with <strong>threads + futex</strong>, <strong>synchronization primitives</strong>, the <strong>energy (C-state) + SMP governor</strong>, <strong>TLS 1.3 (Rustls)</strong>, <strong>ECDSA P-256 module signing</strong>, the <strong>TCP/UDP/DNS stack (smoltcp)</strong>, the in-kernel <strong>802.11 station engine</strong>, the <strong>DRM/KMS core</strong>, and the <strong>VFS</strong> live in <strong>Rust</strong> crates <code>cact_mm</code>, <code>sched</code>, <code>cact_sync</code>, <code>cact_crypto</code> (vendored <code>rustls</code>/<code>webpki</code>), <code>cact_net</code>, <code>cact80211</code>, <code>cact_drm</code>, and <code>cact_vfs</code>.
</p>

---

## 📊 Stats

| | |
|---|---|
| **Syscalls** | 15 core traps — authoritative enum in [`syscalls.h`](Cact/kernel/core/syscall/syscalls.h) (`SYSCALL_COUNT`); everything else via VFS-node ioctls ([`ioctl_abi.h`](Cact/kernel/core/syscall/ioctl_abi.h)) |
| **CPU ISRs** | 32 (IDT) + IRQ stubs via I/O APIC |
| **PMM range** | All usable frames below 4 GiB, up to the **runtime** PCI-hole ceiling `pmm::ram_end()` from the Multiboot2 map (default `0xC000_0000`); arrays sized for the 4 GiB worst case |
| **MAX_FD** | 256 file descriptors per task (`rust_mm` FFI) |
| **Kernel sockets** | `KSOCK_MAX` 16 VFS socket nodes; `TCP_MAX_SOCKETS` 8; `UDP_SOCK_MAX` 8 (see `rust_net` / `tcp.h`); blocking TCP `connect`/`read`/`write`, `getsockname`/`getpeername`, `O_NONBLOCK`, `-EINTR` on a pending signal |
| **xHCI** | ~32 KiB host stack (USB 3.x) |
| **Scheduler** | 4-level MLFQ (Rust) |
| **Threads** | Per-process **thread groups** (`CACT_PROCCTL_THREAD_CREATE`) sharing `ProcShared`; raw **`futex`** (private/shared), per-thread TLS via `%gs`, `clear_child_tid` join |
| **CPU topology / energy** | CPUID `0xB`/`0x1F` package/core decode; master+worker C-state governor (C1/C3/C6), `IPI_HALT`/`IPI_WAKEUP`, SMP worker bring-up |
| **Wi-Fi** | In-kernel **802.11 STA engine** `cact80211` (Rust): scan, open auth/assoc, 802.11↔802.3 datapath, software CCMP; `wljoin` = userspace WPA2 supplicant; `/dev/wlan0` ABI (`CACT_WLANCTL_*` 0x3F00) |
| **Loopback** | `lo` 127.0.0.1/8 is always present — the only interface when no NIC is loaded, else a second address on the NIC |
| **GPU / DRM / KMS** | In-tree Rust `cact_drm` core (`Cact/drivers/gpu`): GEM on memfd, dumb buffers, mmap, atomic modeset, planes, cursors, PRIME, syncobj, events; `/dev/dri/cardN` + `renderDN`; display drivers out-of-tree as **`.cctk`** |
| **ext4** | Out-of-tree **`.cctk`** module (`EXT4-for-Cact-x86_32`) — read/write, inode operations |
| **TLS 1.3** | Rustls-based, in-kernel |
| **Task struct** | 48 bytes; thread state lives in `ProcMeta`/`ProcShared` |

---

## 🔗 Ecosystem & full-disk workflow

CactKernel is one piece of a larger workspace. Typical pieces:

| Component | Role |
|-----------|------|
| **[CactLib-x86_32](https://github.com/QwaYer/CactLibc-x86_32)** | Userspace **`clibc.so`** (plus the **`ld.so`** dynamic linker). Every `SYS_*` number must match the kernel’s [`syscalls.h`](Cact/kernel/core/syscall/syscalls.h) and `ioctl_abi.h`. After any syscall change: rebuild libc and **re-link all ELFs** (init, shell, demos). |
| **[LocalRepoCactOS](../LocalRepoCactOS-x86_32)** | Builds relocatable **`.cctk`** PCI modules, stages ELF binaries under **`lib/bin/`**, and packs a single GRUB module **`cctkfs.img`**. GRUB loads it as `module2 /boot/cctkfs.img cctkfs` (see [`grub.cfg`](grub.cfg)). |
| **[`CactBridge-x86/build.py`](../CactBridge-x86/build.py)** | One-shot: rebuilds every sibling (drivers → **`cctkfs.img`** → kernel) and packs the bootable **`build/cact-non-gui.iso`** / **`cact-gui.iso`**; **`--run`** boots it in QEMU (the empty **ext4** disk is created by [`build_disk.sh`](build_disk.sh) on first run). |

**Why `cctkfs` exists:** the kernel copies the Multiboot2 "cctkfs" module into a large **`.bss`** staging buffer **before paging** (`initfs_modblob_load`). At runtime, **binfs / sbinfs / libfs / usrfs** overlay files from that archive on top of ext4 (e.g. **`/usr/bin/init`**, **`/usr/lib/clibc.so`**, optional **`/usr/lib/modules/*.cctk`** drivers), and the bare **`/bin`, `/sbin`, `/lib`** are symlinks into **`/usr`**. PCI dynamic loading reads ET_REL blobs from the same archive. All modules are verified with an **ECDSA P-256** signature against the kernel's embedded public key (plus a ksym-ABI `vermagic`) before loading.

The **`localrepo`** module is a **second** archive (`localrepo.tar.zst`), too big to stage: the kernel keeps it in place on the boot medium and serves it through **`/dev/localrepo`** (compressed), so `firmextr` and package tooling can stream it without copying it into RAM.

From the workspace root (QEMU-oriented full rebuild):

```sh
python3 CactBridge-x86/build.py --non-gui-iso      # full ISO (+ QEMU with --run)
# Kernel only (expects ../LocalRepoCactOS-x86_32/cctkfs.img already packed):
cd CactKernel-x86_32 && meson setup build-meson --cross-file cross/i686-cact-clang.ini && ninja -C build-meson
```

Hardware-focused ISO without relying on a root disk layout: `meson configure build-meson -Diso_grub_cfg=grub.cfg.ramroot` before building `cact.iso`.

---

## 🔨 Building

**Recommended — full workspace**

Use sibling **[CactOS-x86_32](https://github.com/QwaYer/CactOS-x86_32)**: from the common parent directory run **`ninja -C CactOS-x86_32/build-meson stage`** (or **`… iso`**) — **CactOS** drives **CactLibc**, user programs, **LocalRepoCactOS**, this kernel, and **CactBridge**.

**Standalone — this repository**

**Toolchain**

| Tool | Notes |
|------|-------|
| `clang -m32` | Compiles the kernel; freestanding, so no 32-bit libc is needed |
| `nasm` | Multiboot2 entry + interrupt stubs |
| `ld -m elf_i386` | GNU binutils — the default linker (`-Dlinker=lld` works once `BLOCK(4K)` leaves `linker.ld`) |
| `meson` + `ninja` | Drive the build; every source directory owns a `meson.build` |
| `cargo +nightly` | Builds the kernel Rust graph — `cact_mm`, `sched`, `cact_drm`, `cact_net`, `cact80211`, `cact_vfs`, `cact_crypto`/`cact_hmac_ffi` — as rlibs with **`-Z build-std=core,alloc,compiler_builtins`** and the **`i686-cact`** JSON target |
| `grub-mkrescue` + `xorriso` | produces **`build-meson/cact.iso`** |
| `qemu-img`, `mkfs.ext4`, `e2fsck` | For [`build_disk.sh`](build_disk.sh) |

**Build (Meson + Ninja)**

Every source directory owns a `meson.build` and produces its own static library; the C sources are compiled by `clang -m32` (freestanding) and linked against [`linker.ld`](linker.ld).

```sh
meson setup build-meson --cross-file cross/i686-cact-clang.ini
ninja -C build-meson             # kernel.bin + cact.iso (kernel-only GRUB)
ninja -C build-meson iso-full    # cact-full.iso with cctkfs.img + localrepo.tar.zst
ninja -C build-meson check       # headless QEMU boot check (COM1: userspace, vermagic, localrepo)
ninja -C build-meson clean
ninja -C build-meson rust-clean  # cargo clean in the four Rust crates
ninja -C build-meson acpica-clean
./build_disk.sh                  # empty ext4 build/nvme.img for ./run_qemu.sh
```

Useful options: `-Dkern_debug=true` (`-g -Og`, for GDB), `-Dlinker=ld|lld`, `-Dlocal_repo=/path` (where `cctkfs.img` lives), `-Dacpica_fetch=false` (never clone ACPICA during configure).

**ISO with bundled `cctkfs.img` (`iso-full`)**

```sh
meson configure build-meson -Dlocal_repo=../LocalRepoCactOS-x86_32
ninja -C build-meson iso-full   # signs cctkfs.img via tools/cact_sign_cctkfs.py
```

`iso-full` is a run_target: it always regenerates, and fails loudly when
`cctkfs.img` is missing. It rides **two** GRUB modules on the ISO — the **`cctkfs`**
archive and the compressed **`localrepo.tar.zst`** archive (served in place as
`/dev/localrepo`, so `firmextr` and package tooling have something to read).

**QEMU:** [`./run_qemu.sh`](./run_qemu.sh) picks up **`build-meson/cact-full.iso`** (then **`build-meson/cact.iso`**); override with **`CACT_ISO`**.

> 🧩 **`python3`** is only needed to pack **`cctkfs.img`** in LocalRepoCactOS and for **`tools/cact_sign_cctkfs.py`** — not for the default kernel build (`grub.cfg.kernelonly`).

**Successful build footer** (version from [`VERSION`](VERSION), commit from `git`) — printed by `meson setup`:

```
  Cact kernel 2.0.0+abi.0x87fb66d1.i686 (<short>)
  compiler: clang 22.1.8
  linker  : /usr/bin/ld

  ninja -C build-meson          -> kernel.bin + cact.iso
  ninja -C build-meson iso-full -> cact-full.iso
```

**Version macros** (from [`meson.build`](meson.build)):

```meson
'-DCACT_VERSION=' + cact_version,
'-DCACT_COMMIT_HASH=' + cact_commit,
```

`kernel_build_time` is not a configure-time macro: `Cact/kernel/core/kern_ver/meson.build`
generates `cact_build_time.c` on every build (`custom_target`, `build_always_stale`),
so the banner reports the actual build time instead of the last `meson setup`.

**Final link** (simplified): all C objects + the kernel's single Rust archive **`libcact_kernel.a`** (built by `Cact/kernel/kernel_rust`) — every kernel Rust crate is an rlib pulled into it: **`cact_mm`** (PMM/VMM/brk/mmap/memfd), **`sched`** (MLFQ, threads, futex, energy governor, SMP), **`cact_net`** (smoltcp, virtio PHY shim, ICMP, ARP, DNS resolver, TCP/UDP socket glue, loopback), **`cact80211`** (the in-kernel 802.11 station engine), **`cact_drm`**/**KMS**, **`cact_vfs`**, **`cact_crypto`** (the vendored **rustls**/**webpki** provider), and **`cact_hmac_ffi`** (HMAC/HKDF behind `/dev/crypto`). Loadable-module ECDSA verification lives in C (`Cact/kernel/elf/mod_tag.c`) and calls the `cact_crypto` verifier. Link script: [`linker.ld`](linker.ld) with **`-z noexecstack`**.

Optional: `meson configure build-meson -Dkern_debug=true` for richer symbols; QEMU GDB: see [`run_qemu.sh`](run_qemu.sh).

---

## 📂 Repository layout

```
CactKernel-x86_32/
├── Cact/
│   ├── kernel/
│   │   ├── core/        kernel entry, Multiboot2, syscall dispatch (sysenter), IRQ, klog, klib
│   │   ├── cpudev/      CPUID feature/vendor detection, syscall mechanism select (SEP vs AMD)
│   │   ├── memory/      rust_mm/ — PMM, VMM, page faults, mmap, memfd, swap, slab, SHM
│   │   ├── proc/        task_struct, context switch, task.h, sched/ (Rust: MLFQ, threads, futex, energy, SMP)
│   │   ├── sync/        locks, semaphores, sleeping mutexes (Rust + C FFI)
│   │   ├── elf/         static ELF loader, ksym/sym, module tag verify (ECDSA + vermagic)
│   │   ├── energy/      C-state governor interface (impl in Rust sched/src/energy.rs)
│   │   ├── smp/         AP trampoline + per-CPU GDT/TSS bring-up
│   │   ├── gdt/ idt/
│   │   └── kernel_rust/ the single Rust staticlib (cact_kernel) linking every kernel rlib
│   ├── drivers/
│   │   ├── acpi/        ACPICA engine — AML interpreter, MADT/APIC/FADT tables, C-states, power
│   │   ├── block/       blkdev, page cache, partition scan (MBR/GPT)
│   │   ├── firmware/    request_firmware()/release_firmware() for modules (cctkfs blobs, .zst)
│   │   ├── gpu/         DRM uapi + KMS core (crates kms + rust_drm) — GEM, dumb buffers, modeset
│   │   ├── input/       USB HID only (PS/2 removed in 2.0)
│   │   ├── timer/       ktime (TSC primary, ACPI PM fallback), LAPIC scheduler tick, CMOS RTC, tick counter
│   │   ├── initfs/      cctkfs staging + module blob reader
│   │   ├── pci/         enumerator, PCIe ECAM, MSI/MSI-X, ELF module loader, mod_tag verify
│   │   ├── quirks/      per-machine workarounds (acpi DMI, xhci PCI-ID flags; pci/ empty)
│   │   ├── tty/         VT console + line discipline (Alt+F<n>)
│   │   ├── usb/         xhci + HID + hub
│   │   ├── video/       framebuffer console, PSF2 font parser, PAT WC + shadow blit
│   │   └── zstd/        kernel zstd decompressor (firmware `.zst` blobs)
│   ├── crypto/          vendored rustls + webpki, cact_crypto provider, cact_hmac_ffi, modsign key
│   ├── fs/
│   │   ├── rust_vfs/    Rust VFS core, struct file, dcache, generic page cache, pipe
│   │   ├── vfs/         C filesystem implementations — devfs, procfs, mntfs, etcfs, tmpfs,
│   │   │                binfs, sbinfs, libfs, usrfs, varfs, cctkfs_tree, vfsdev, fs_mod
│   │   ├── mqueue/      POSIX message queues (queue filesystem mounted at /dev/mqueue)
│   │   ├── pty/         /dev/ptmx + /dev/pts/<n>
│   │   ├── pipe/        /dev/pipe registration (the pipe itself lives in rust_vfs)
│   │   └── memfd/ eventfd/ timerfd/ signalfd/ epoll/   fd-type singletons behind /dev/<name>
│   └── net/
│       ├── rust_net/    pure-Rust stack (smoltcp: Ethernet/ARP/IP/ICMP/TCP/UDP/DNS) + rustls TLS + HTTP(S) client, C FFI header
│       ├── wireless/    cact80211 — in-kernel 802.11 STA engine (scan/auth/assoc/datapath/CCMP)
│       ├── socket/      socket VFS node type (impl under kernel/core/syscall/sock/)
│       └── protocols/   C TCP/UDP metadata headers kept for the module ABI
├── meson.build
├── VERSION
├── linker.ld
├── grub.cfg              # multiboot2 kernel + cctkfs + localrepo modules
├── grub.cfg.kernelonly   # kernel-only multiboot (the cact.iso default, -Diso_grub_cfg)
├── grub.cfg.ramroot      # RAM-first userland variant
├── build_disk.sh         # raw ext4 image for QEMU AHCI/NVMe
├── tools/                # utility scripts (modsign.py, gen_module_keys.py, cact_sign*.py, cact_check.sh, cact_make_iso.sh, …)
└── run_qemu.sh           # launches QEMU; runs build_disk.sh if nvme.img missing
```

---

## 🚀 Boot sequence

Boot is split into **three phases**: early `init()` (identity map, no user IRQs yet), **`kernel_setup_hardware()`** (bring up devices and subsystems), then a **bootstrap kernel thread** that can sleep on semaphores while mounting storage.

### Phase A — early `init()` (still single stack, interrupts globally masked)

| Step | What happens |
|------|----------------|
| 1 | **Multiboot2** parse — memory map, framebuffer tag, modules (the `localrepo` archive is recorded separately and served in place) |
| 2 | **cctkfs staging** — `initfs_modblob_load()` copies the GRUB "cctkfs" module from its physical address into kernel **`.bss`** before paging is enabled |
| 3 | **Framebuffer** — `fb_init()`; if no FB tag / zero size → halt (blind) |
| 4 | **Console font** — `font_load_boot()` parses the PSF2 file staged in cctkfs; a missing/broken font is a **panic** (the kernel cannot draw a glyph) |
| 5 | Magic check (`0x36D76289`) |
| 6 | **`kernel_setup_hardware()`** — see Phase B |
| 7 | **`create_task(kernel_bootstrap_main)`** — deferred work that needs the scheduler |
| 8 | **`sti`** — boot thread becomes the **idle** task (HLT loop); the LAPIC timer IRQ drives preemption, with a **2 s no-tick watchdog** that dumps LAPIC state and halts loudly instead of freezing silently |

### Phase B — `kernel_setup_hardware()`

Order matters (e.g. **blkdev** before PCI so AHCI/NVMe can register).

| # | Subsystem |
|---|-----------|
| 1 | **GDT** → **CPU feature detection** (`cpudev_init`) → **x87 FPU + SSE** (`CR4.OSFXSR`; the compiler needs XMM for 64-bit ops) → **PMM boot-image reservation** → **PMM** (from MB2 mmap) → **VMM** → **kmalloc heap** → **paging on** |
| 2 | **Slab allocator** + **page fault** handler (COW, demand zero, swap markers) |
| 3 | **IDT** (256 vectors) + fast-syscall MSRs (`sysenter`/`syscall`) + **COM1 serial** (mirrors part of `kprint` / `klog` to host) |
| 4 | **Linear framebuffer** console + **PAT** write-combining for VRAM + **WB shadow** buffer, then terminal winsize from FB geometry |
| 5 | **ACPI** — parse RSDP, MADT, FADT tables via ACPICA |
| 6 | **Timekeeping** — TSC (ACPI PM timer fallback) + **LAPIC timer @ 100 Hz** scheduler tick; **CMOS RTC** civil wall clock; the **ksym vermagic** line |
| 7 | **I/O APIC** up; **ACPI power path** (S5 soft-off, reset register) |
| 8 | **Energy governor** — master/worker core map, C-state controller + `IPI_HALT`/`IPI_WAKEUP` vectors, per-tick load monitor, decision engine, **per-core runqueues + balancer (cross-runqueue migration)**; **CPU topology** (CPUID `0xB`/`0x1F`) for `/proc/cpuinfo` |
| 9 | **SMP** — `smp_init()` wakes the worker cores, each of which loads its per-CPU GDT/TSS |
| 10 | **`msidev_init`** (MSI/MSI-X vector pool) → **`swap_init`** (I/O callbacks only — swap stays off until a partition appears) → **`blkdev_init`** → **partition probe** (MBR/GPT) → **PCIe ECAM** → **PCI scan** + **enumeration** → **`usb_init`** (xHCI) |
| 11 | **Process cache** (`pc_init`) + **`vfs_init`** |
| 12 | **`task_init`** + **`init_scheduler`** (Rust MLFQ) |
| 13 | **`net_init`** — Rust `stack_init`, **`net_poll_task`** (semaphore, woken by NIC RX) + **`net_timer_task`** (advances smoltcp timers without RX) |
| 14 | **Deferred tasks** spawned once the scheduler exists: USB-HID key repeat, HID indicator LEDs, VT console switch (`Alt+F<n>`), xHCI root-port hotplug, USB-hub hotplug |

### Phase C — `kernel_bootstrap_main` (first real task)

| # | Action |
|---|--------|
| 1 | **`pcidev_probe_all()`** — attach/scan PCI devices now that a task context exists (module loading itself is a **userspace** concern — nothing is auto-loaded) |
| 2 | **`mntfs_init`** — parse mount table, **mount ext4** on NVMe/AHCI (may **`sema_down`** waiting for IRQ completions — **illegal** from the raw boot stack, hence this thread). With no filesystem module it falls back to a **virtual nodisk root** and the kernel still boots to `/usr/bin/init` |
| 3 | **`create_elf_task("usr/bin/init")`** — first userspace process; binary resolved through **binfs** (ext4 `/usr/bin` + **cctkfs** overlay), then the console log level drops to `NOTICE` |

**Typical serial / FB banner:**

```
Cact Kernel 2.0.0+abi.0x87fb66d1.i686
--------------------------
[VER] commit=…  built=…
Kernel is ready. Launching init…
```

---


---

## 🧠 Memory map (`rust_mm`)

The PMM installs every **available** frame below 4 GiB that the **Multiboot2 memory map** reports, up to a **runtime** ceiling `pmm::ram_end()` — the page-aligned top of usable RAM below the PCI hole (default `PCI_HOLE_START`, `0xC000_0000`, but e.g. `0xE000_0000` on a board whose hole is at 3.5 GiB) — and below **`USER_FRAME_CEIL` (`0x8000_0000`)**: the kernel reaches a frame through its *identity* VA, and a process page directory inherits that identity map only as shared page tables, so a user mapping inside a VA window would replace the kernel's view of a live frame. Every user VA window therefore lives at or above that ceiling, and RAM above it is left unmanaged (a 3 GiB board keeps 2 GiB). The static arrays are sized for the worst case (**`TOTAL_PAGES = 1 048 576`**, 4 GiB) and the user/kernel **VA** split (`USER_STACK_TOP`) is independent of the RAM ceiling. Frames inside the **low 512 MiB** reservation (BIOS, kernel image incl. the 24 MiB cctkfs staging buffer, static page tables, *and the low user VA region* — ELF images, libc, ld.so, the brk heap) are permanently marked used; the PMM then raises that boundary past the Multiboot2 module RAM (`pmm_reserve_boot_images`).

| Symbol | Value | Meaning |
|--------|-------|---------|
| `MEM_START` | `0x00100000` | Conventional kernel load floor |
| `PCI_HOLE_START` | `0xC0000000` | Default RAM ceiling / VA split; the effective ceiling is `min(pmm::ram_end(), USER_FRAME_CEIL)` at boot |
| `TOTAL_PAGES` | `1024 * 1024` | Frame arrays sized for 4 GiB (worst case) |
| `BITMAP_SIZE` | `TOTAL_PAGES / 8` | Bitmap byte count (128 KiB worst case) |
| `USER_FRAME_CEIL` | `0x80000000` (2 GiB) | Highest physical address handed out; lowest user VA window (`MMAP_BASE`) |
| `RESERVED_END` | `0x20000000` (512 MiB) | Low memory never given to `kalloc`/user (floor; the boot modules raise it past their own RAM) |
| `HEAP_START` / `HEAP_SIZE` | `0x20000000` / 16 MiB | Kernel heap window (floor; the PMM raises it above the boot modules) |
| `HEAP_MAGIC` | `0xDEADBEEF` | Heap block canary |
| `MMIO_WINDOW_BASE` | `≥ pmm::ram_end()` | Base of driver-owned MMIO mappings (placed above usable RAM so a WC/UC window cannot shadow a live frame) |
| `SWAP_MAX_SLOTS` | 65536 | Swap bitmap. **Swap is opt-in per boot:** `swap_init` only installs the I/O callbacks (`swap_disk_read`/`write`) and swap stays off until the partition layer finds a swap partition and calls `swap_enable_device` — with no swap device nothing reaches the block layer and `/proc/meminfo` reports no swap |
| `SLAB_MIN/MAX` | 8 … 2048 B | Slab object sizes |

### User virtual layout (reference)

```
0xC0000000  ┌──────────────────────────┐  Kernel-only (ring 0)
0xBF000000  ├──────────────────────────┤  User stack floor
            │       user stack ↓        │
0xBEFFF000  ├──────────────────────────┤  Per-process sigreturn trampoline page (sysenter stub)
0xB0000000  ├──────────────────────────┤  SHM ceiling (`SHM_VA_LIMIT`)
0xA0000000  ├──────────────────────────┤  SHM base (`SHM_VA_BASE`)
0x80000000  ├──────────────────────────┤  mmap window base (`MMAP_BASE`) == `USER_FRAME_CEIL` (no PMM frame at/above this)
            │       mmap regions ↓      │  up to 256 regions (`MMAP_LIMIT` = 0xBF000000)
0x18000000  ├──────────────────────────┤  ld.so / `PT_INTERP` (fixed base, kernel maps it at its link vaddr)
0x10000000  ├──────────────────────────┤  libc (`CactLibc/libc.ld`)
0x08000000  ├──────────────────────────┤  PIE image base (`USER_SPACE_START`); the brk heap grows above it
            │   brk heap ↑ (≤16 MiB)   │
0x00000000  └──────────────────────────┘  NULL / guard
```

Everything below `RESERVED_END` (0x20000000) is reserved, so the low user
regions (PIE images, libc, ld.so, brk heap) can never share an address with a
handed-out frame; every region at or above `USER_FRAME_CEIL` is clear of the
PMM by construction.

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
- **Sleep queue** + **alarms** / **`setitimer`** hooks run on each timer tick; a **timer wheel** (`timer_wheel.rs`) holds the deadline heap.
- **Re-entrancy guard** (`SCHEDULE_IN_PROGRESS`) prevents nested scheduler entry.

**Task states:** `TASK_READY`, `TASK_RUNNING`, `TASK_SLEEPING`, `TASK_ZOMBIE`, `TASK_WAITING`.

### Threads, futex & blocking primitives

A task created with **`CACT_PROCCTL_THREAD_CREATE`** is a member of the calling
task's **thread group**: it shares the address space and fd table (`ProcShared`)
but gets its own kernel stack, its own `%gs` thread pointer
(`CACT_PROCCTL_SET_TLS`) and an optional `clear_child_tid` join word — `thread_exit`
zeroes that word and wakes a `futex(WAIT)` joiner. `gettid`/`thread_exit` are the
rest of the surface. Raw **`futex`** wait/wake (private, keyed by address space +
address; shared, keyed by physical address) backs the libc's mutexes and
semaphores, alongside the kernel's own sleeping mutexes and counting semaphores
(`cact_sync`). Threads are real **tasks** in the MLFQ — `tgid == pid` for a group
leader, `is_thread = 1` for the rest.

---

## 🔋 Energy governor & SMP

Beyond the scheduler, a **master/worker core model with a C-state governor**
(`Cact/kernel/energy`, implemented in Rust `sched/src/energy.rs`) decides how
deeply each core may idle:

- **Topology** — CPUID leaf `0xB`/`0x1F` (falling back to leaf `4` + leaf `1`
  HTT, then MADT order) decodes `package_id`/`core_id`/`smt_sibling_mask`;
  `/proc/cpuinfo` reports `core id`, `physical id`, `siblings`, `cpu cores`.
- **Master + workers** — `cpu0` is the master (always C0); the other present
  cores are workers the governor may park (C6) or wake with an IPI.
- **C-states** — C1 is `HLT`; deeper states (C3/C6, mapped onto MWAIT hints) are
  entered only when ACPI supplies them via `_CST` (otherwise the controller logs
  `C3/C6 pending _CST`). Deep states are gated on *all* siblings of a physical
  core being idle, and a whole physical core is parked/offlined as one unit. The
  controller re-arms IDT vectors `0xF8`/`0xF9` for `IPI_HALT`/`IPI_WAKEUP`.
- **MLFQ ↔ C-state map** — the queue class a task runs in bounds the idle depth
  the core may enter: `RT=C0, interactive=C1, normal=C3, background=C6`.
- **Per-core runqueues** — the scheduler keeps a **ready queue per logical CPU**:
  a task is enqueued on the core that makes it runnable, an idle core pulls from
  the busiest peer when its own queue is empty (work stealing), and the master's
  balancer moves one ready task per tick across a load spread (`balance_policy`
  decides, `energy_balance_migrate` moves it), waking a deep-idle destination
  first. **Placement** still prefers a fresh physical core over the sibling of a
  busy one (`placement.rs`).  There is no CPU-affinity API, and a woken task is
  queued on the waker's core rather than a chosen one.
- **SMP bring-up** (`Cact/kernel/smp`, `smp.rs`) wakes the worker cores, each of
  which loads its per-CPU GDT/TSS — the boot core keeps every global scheduler
  lock until `sched_boot_complete()`, after which workers dispatch tasks.
- **S3 suspend/resume was removed in 2.0.0**; poweroff, reboot and halt remain.

---

## 💾 Drivers (in-tree)

| Area | Components | Notes |
|------|------------|-------|
| **Block** | blkdev, page cache, **partition scan** (MBR/GPT) | The block layer is in-tree (`BLKDEV_MAX_DISKS` 8, 16 partitions/disk); the **AHCI**/**NVMe** drivers themselves ship as **`.cctk`** (sibling repos, `AHCI-for-Cact-x86_32` / `NVMe-for-Cact-x86_32`) |
| **USB** | xHCI, HID, hub | ~32 KiB host stack (USB 3.x); root-port and hub hotplug run in dedicated tasks — PS/2 removed in 2.0 |
| **Input** | USB HID keyboard & mouse | Software key repeat (SET_IDLE(0)) and indicator LEDs (Num/Caps/Scroll) |
| **Video** | Linear FB 32 bpp, PSF2 console font from cctkfs (`/usr/share/consolefont.psf`, ×2 scale), PAT WC + WB shadow blit | |
| **GPU / DRM / KMS** | In-tree DRM uapi + KMS core (`Cact/drivers/gpu`; Rust `rust_drm` + `kms`): GEM, dumb buffers, mmap, atomic modeset, planes, cursors, syncobj, PRIME | No display driver in-tree — virtio-gpu / Intel ship out-of-tree as **`.cctk`** |
| **TTY / VT** | VT console `/dev/ttyN` (`tty0` = the active VT), allocated on demand up to `TTY_MAX_VT` 63, each with `termios` + line discipline (canonical/raw) and a replay ring; `Alt+F<n>` switch | |
| **Firmware** | `request_firmware()` / `release_firmware()` (ksym) — stage a blob from **cctkfs**, transparently **`.zst`**-decompressed | Build-time table only (no on-demand loader/workqueue) |
| **Zstd** | In-kernel zstd decompressor (`Cact/drivers/zstd`, vendored from Linux `lib/zstd`) | Used by the firmware loader to expand `.zst` blobs (whole-buffer, single-frame, ≤4 GiB) |
| **PCI** | Config scan, **PCIe ECAM**, MSI/MSI-X vector pool, driver table, **modblob** loader, ECDSA P-256 + vermagic verification | Loads ET_REL modules from **cctkfs** or path (user-driven via kmod syscalls) |
| **Quirks** | Per-machine workarounds: `quirks/acpi` (DMI board quirks) and `quirks/xhci` (PCI-ID flag table) | `quirks/pci/` is currently empty |
| **Network** | **virtio-net** (`.cctk`); in-kernel **802.11** engine (`cact80211`) for Wi-Fi radios | Default NIC under QEMU; other NICs packaged as **`.cctk`** (e.g. Marvell **Yukon**); **RT3070** USB Wi-Fi via the `RT2800USB` module, whose driver is now a *dumb radio* |
| **ACPI** | ACPICA — RSDP, MADT, FADT, APIC table parsing, `_CST` C-states, power (S5/reset) | Since 2.0 |

All out-of-tree PCI drivers register their interrupt through the kernel's **`msidev_register()`** — **MSI-X** when the device offers it, otherwise a single **MSI** message — instead of PIC IRQ lines; per-machine exceptions live under `Cact/drivers/quirks/`. Extra PCI drivers live in **`*-for-Cact-x86_32`** repositories; **`ninja -C CactOS-x86_32/build-meson drivers`** (workspace integrator) installs them into **`LocalRepoCactOS-x86_32/lib/`** and packs **`cctkfs.img`**.

---

## 🖼️ Graphics — DRM / KMS

The whole DRM/KMS core is in-tree and in **Rust** (`Cact/drivers/gpu`, crate
`cact_drm`: `drm/rust_drm` plus the sibling `kms/` pulled in with `#[path]`,
since both halves share `DrmDevice`/`DrmFile`). What is left in **C** is the ABI
only — the uapi headers (`drm.h`, `drm_mode.h`, `drm_ioctl.h`, `drm_fourcc.h`)
and [`drm_drv.h`](Cact/drivers/gpu/drm/drm_drv.h), which a display driver
compiles against because that driver is a separate C **`.cctk`**. The first
device to register publishes **`/dev/dri/cardN`** (mode `0600`) and
**`/dev/dri/renderDN`**; the console is the GRUB framebuffer (`/dev/fb0`, see
**Video**) and does not go through DRM.

- **Device & client model** — `drm_dev_create()` (alloc + register) publishes
  the node and runs the driver's `load()`. Every `open()` gets a client of its
  own — GEM handle table, framebuffers, syncobjs, event queue — kept in
  `file_t.priv` and released in one order on close (driver `close` first, then
  handles, then framebuffers). The first **`cardN`** open becomes **master**; a
  render node never does. Objects, handles, mmap offsets, properties, EDID
  blobs and queued events are all allocator-backed, so nothing is capped.
- **KMS** — CRTCs, encoders, connectors (mode list + variable-length EDID),
  planes and properties: `GETRESOURCES`, `GETCRTC`/`SETCRTC`, `GETENCODER`,
  `GETCONNECTOR`, `GETPLANE(RESOURCES)`/`SETPLANE`, `ADDFB`/`ADDFB2`/`RMFB`/
  `GETFB2`, `DIRTYFB`, `PAGE_FLIP`, `CURSOR`/`CURSOR2`, `GETPROPERTY`/
  `GETPROPBLOB`/`CREATEPROPBLOB`/`DESTROYPROPBLOB`, `OBJ_GETPROPERTIES`/
  `OBJ_SETPROPERTY`, and the **atomic** request (`ATOMIC`: mode objects +
  blobs). The vendor-neutral ioctls (`VERSION`, `GET_CAP`, `SET_CLIENT_CAP`,
  magic, master/auth, `GEM_*`, `WAIT_VBLANK`) sit in the same dispatcher, and
  every command at or above `DRM_COMMAND_BASE` is forwarded to the driver's own
  `ioctl`.
- **GEM** — an object is **memfd-backed** (see **Filesystems**), which is what
  lets `mmap`, `fork` and PRIME fall out instead of needing a second
  frame-sharing mechanism: dumb buffers (`CREATE_DUMB`/`MAP_DUMB`/
  `DESTROY_DUMB`, the map offset resolved back to the object in the VFS mmap
  hook), `GEM_FLINK`/`GEM_OPEN`, per-client handle tables, and
  `drm_gem_page_phys()` / `drm_gem_page_count()` for a driver that programmes
  the device to read the object (scanout, a GPU page table).
- **PRIME** — `HANDLE_TO_FD` / `FD_TO_HANDLE` move an object between processes
  as an fd; the fd *is* the object's storage (no separate dma-buf layer).
- **Syncobj** — **binary** and **timeline** objects: create/destroy,
  `HANDLE_TO_FD`/`FD_TO_HANDLE` (an exported object becomes a small fd that
  polls readable once signalled — the sync-file shape, so it can be handed to
  another process or to `poll()`), `WAIT`/`TIMELINE_WAIT`, `SIGNAL`/
  `TIMELINE_SIGNAL`, `RESET`, `QUERY`, `TRANSFER` (a signalled source onto a
  binary destination, or a point onto a timeline one) and `EVENTFD` (an eventfd
  incremented once when the object reaches a point — the registration holds the
  eventfd open, so it fires even after the registering process closed that fd).
  The core holds no *pending* fences: an object is signalled or not and its
  point reached or not, so `WAIT_AVAILABLE` and its absence coincide and a
  transfer's source must have reached the state being moved.
- **Vblank & events** — a driver with a real vblank interrupt enables it and
  calls `drm_crtc_handle_vblank()`; a driver without one (virtio-gpu) leaves
  the hook NULL and the core **models** vblanks from the monotonic clock and
  the mode's refresh rate, so `WAIT_VBLANK` and `PAGE_FLIP_EVENT` keep making
  progress. `read(2)` on a card fd drains the queue in the `drm_event_vblank`
  shape libdrm's `drmHandleEvent()` expects, and `poll` reports readable while
  it is non-empty.
- **Driver contract** — `drm_driver_ops_t`: `load`/`unload`, `mode_valid`/
  `set_config`/`page_flip`/`dirty`, `enable_vblank`/`disable_vblank`,
  `gem_create`/`gem_free`, `cursor_set`/`cursor_move`, `close`, and an optional
  private `ioctl` — where virtio-gpu keeps its `DRM_IOCTL_VIRTGPU_*` range and
  the 3D path. Every callback is optional: a driver that only reports modes
  leaves scanout NULL and the core keeps the state. A driver never talks to
  userspace; ioctl decoding, handle tables, mmap offsets and mode/EDID
  marshalling all live in the core. The ~34 entry points it needs are exported
  through the ksym table (`drm_dev_*`, `drm_mode_*_init`, `drm_crtc_find`/
  `drm_connector_*`, `drm_gem_*`).

Display drivers ship out-of-tree as **`.cctk`** modules bound by PCI
vendor/device ID — **virtio-gpu** (QEMU `-device virtio-vga`,
[`Virtio-gpu-for-Cact-x86_32`](../Virtio-gpu-for-Cact-x86_32)) and **Intel UHD
630** ([`Intel-GPU-for-Cact-x86_32`](../Intel-GPU-for-Cact-x86_32): bind, EDID,
linear scanout, vblank IRQ, hardware cursor) — and nothing is auto-loaded:
`/dev/dri` appears only once userspace loads one.

---

## 📁 Filesystems

| FS | Status | Notes |
|----|--------|-------|
| **ext4** | Active (**`.cctk`**) | Out-of-tree module ([`EXT4-for-Cact-x86_32`](https://github.com/QwaYer/EXT4-for-Cact-x86_32)) — read/write, inodes; loaded from **cctkfs** |
| **VFS** | Active | Up to **`VFS_MOUNT_MAX` (32)** simultaneous mount points, symlink pool with **ELOOP** detection, `rwx` permission bits, `struct file` with fd table |
| **devfs** | Active | Device nodes as VFS files |
| **procfs** | Active | e.g. `/proc/cmd`, `meminfo`, module listings |
| **mntfs** | Active | User-visible mount table + auto-mount policy at boot |
| **etcfs** | Active | passwd-like uid ↔ name mapping |
| **tmpfs** | Active | RAM-backed files |
| **posixmq** | Active | POSIX message queues — a small filesystem mounted at **`/dev/mqueue`** whose files *are* queues (`Cact/fs/mqueue`, `CACT_MQCTL_*`): `mq_open` is open-or-create, `mq_unlink` is `unlink`, and send/receive (prioritised) are ioctls on the descriptor |
| **binfs** | Active | **`/usr/bin`** + a synthesized **cctkfs** tree (**`cctkfs_tree`** builds the archive hierarchy); **`/bin`** → **`/usr/bin`** symlink |
| **sbinfs** | Active | **`/usr/sbin`** + synthesized cctkfs tree; **`/sbin`** → **`/usr/sbin`** symlink |
| **libfs** | Active | **`/usr/lib`** + synthesized cctkfs tree — **`modules/`**, **`firmware/`**, **`cactpkg/repo/`** and the whole **`cact-install/`** tree are reachable; **`/lib`** → **`/usr/lib`** symlink |
| **usrfs** | Active | **`/usr`** + synthesized cctkfs tree (**`include/`**, **`share/`** and anything nested); **`bin`/`sbin`/`lib`** belong to their own mounts |
| **varfs** | Active | **`/var`** layout |
| **pipes** | Active | `pipe()` integrated with the fd table |
| **memfd / eventfd / timerfd / signalfd / epoll** | Active | fd-type singletons created through `/dev/<name>` (`CACT_MEMFDCTL_*` 0x3800 … `CACT_EPOLLCTL_*` 0x3C00): anonymous memory files, event counters, timers, signal-watching descriptors, and readiness multiplexing (epoll is its own fd with `EPOLL_CTL`/`EPOLL_WAIT`) |

---

## 🌐 Network stack

The **entire L3+ stack is pure Rust** (`cact_net`): Ethernet demux, ARP, IPv4, ICMP, TCP and UDP sockets, and the DNS resolver all run on **smoltcp**; TLS 1.3 runs on the vendored **rustls** with the in-kernel `cact_crypto` provider. The kernel itself has **no DHCP client** and **no compile-time IPv4 default** (`NET_CFG` boots all-zero), so an interface comes up **unnumbered**: IPv4 addressing is decided in userspace (`ip`/`dhcpd`) and pushed in via `/dev/net` `CACT_NETCTL_NETCFG` (`rust_net_set_ipv4_config`) — zeros clear the address/route/resolver. The C side is only a thin FFI layer (`rust_net_ffi.h` + syscall glue) — `net_shim.c` was removed in favour of Rust symbols (`net_receive_packet`, `net_driver_irq_wake`) that out-of-tree NIC modules resolve via the ksym table.

### Interfaces & loopback

There is deliberately **one** smoltcp `Interface` and **one** `SocketSet` (two interfaces sharing one set would each try to dispatch the other's sockets). `CACT_NETCTL_IFACE_COUNT` (`0x3409`) reports **0** (stack down) / **1** / **2**, and `CACT_NETCTL_IFACE_GET` (`0x340A`) returns a `cact_iface_info_t`:

- **no NIC** → `lo` alone (count 1, index 0);
- **NIC + `lo`** → index 0 = the NIC, index 1 = `lo` (count 2). Only **one** NIC is active at a time (`register_netdev` replaces it).

**Loopback** (`lo`, `127.0.0.1/8`) is not a second device: `127.0.0.1/8` is added as a *second address* on the interface (so 127/8 is on-link and smoltcp routes it locally), and the PHY diverts any emitted Ethernet frame that involves 127/8 (IPv4 src **or** dst = 127, or ARP sender/target = 127) into its own RX ring instead of the wire. It needs no NIC module, and it survives a NIC unload (the stack is rebuilt in its NIC-less form).

### ARP / neighbour table

smoltcp keeps its neighbour cache private, so `cact_net` shadows it: `arp.rs` learns mappings passively from the ARP traffic seen in the PHY RX path (`ARP_TABLE_MAX` 16 entries, 60 s lifetime), exposed via `CACT_NETCTL_ARP_COUNT` (`0x340B`) / `CACT_NETCTL_ARP_GET` (`0x340C`) and `cact_arp_entry_t`.

### Wi-Fi — in-kernel 802.11 engine (`cact80211`)

The Linux-shaped split, with the **mac80211-equivalent in the kernel** (Rust crate `cact80211`, `Cact/net/wireless`) and the Wi-Fi driver reduced to a **dumb radio**:

- **Engine** (`hw.rs` / `dev.rs` / `frame.rs` / `ccmp.rs` / `aes.rs` / `control.rs`) — scan (channel walk + probe request), open-system auth/association, the 802.11↔802.3 datapath, TK/GTK install, a beacon-loss watchdog, and **software CCMP/AES-128**. It registers netdev **`wlan0`** and the **`/dev/wlan0`** chardev; `read(/dev/wlan0)` serves the AP list as text.
- **Driver contract** — a NIC module calls the ksym exports **`cact80211_register`** / **`_unregister`** / **`_rx`** with a `cact80211_hw` (ops `tx` / `config` / `rx_poll`) defined in [`cact80211.h`](Cact/net/wireless/cact80211.h); the driver only touches the hardware.
- **Userspace is the WPA2 supplicant** — `wljoin` reads the BSS list, `CONNECT`s, runs the 4-way over EAPOL and installs TK/GTK through `CACT_WLANCTL_*` (`0x3F00`); `wifid` drives the connection lifecycle.
- Current target radio: **Ralink RT3070** (`rt2800usb`, USB) as an out-of-tree **`.cctk`**; the engine stays dormant until that module is loaded on hardware (QEMU has no radio).

Logical TCP states (C metadata / VFS view; ingress TCP is handled by **smoltcp**):

```
CLOSED → LISTEN → SYN_SENT → SYN_RECEIVED
       → ESTABLISHED
       → FIN_WAIT_1 → FIN_WAIT_2 → TIME_WAIT
       → CLOSE_WAIT → LAST_ACK → CLOSED
```

`stack_poll()` drives the iface, **`rust_net_set_ipv4_config`** applies the address/gateway/DNS chosen by userspace, **`CACT_NETCTL_DNS_RESOLVE`** performs a blocking **A-record** query over UDP/53 (with one retransmission before giving up), and **`CACT_NETCTL_PING` / `CACT_NETCTL_PING_WAIT`** send an ICMP echo request and optionally wait for its reply. A periodic `net_timer_task` wakes the poll loop so smoltcp timers (TCP retransmit, timeouts) advance even without RX traffic.

Full socket API (the libc wrappers over **`CACT_SOCKCTL_*`**): `socket`, `bind`, `connect`, `listen`, `accept`, `send`, `recv`, `sendto`, `recvfrom`, `shutdown`, `setsockopt`, `getsockopt`, `getsockname`, `getpeername`.

| Layer | Responsibility |
|-------|----------------|
| **Ethernet / ARP / IPv4 / ICMP / TCP / UDP** | smoltcp (`cact_net`) |
| **Interfaces** | One `Interface` + one `SocketSet`; `lo` = `127.0.0.1/8` as a second address (PHY loopback ring); the `arp.rs` shadow table mirrors smoltcp's private neighbour cache |
| **Wi-Fi** | `cact80211` engine: `wlan0` netdev + `/dev/wlan0` (`CACT_WLANCTL_*`); drivers are dumb radios (RT3070), bound via `cact80211_register` / `_rx` |
| **Sockets / VFS** | Up to **16** kernel socket nodes integrated with `read`/`write`/`close`; `.poll` reports real readiness (`POLLIN`/`POLLOUT`/`POLLHUP`), `fcntl(F_SETFL, O_NONBLOCK)` (and `SOCK_NONBLOCK`) makes `read`/`write` return `-EAGAIN` instead of blocking, `getsockname`/`getpeername` work, and a blocking `read`/`write`/`accept` yields `-EINTR` to a pending signal — so Ctrl+C reaches a task parked in a socket call |
| **net_poll_task** | Dedicated kernel thread: sleeps on a semaphore, wakes on NIC RX, calls **`net_poll` → `stack_poll()`** |
| **net_timer_task** | Periodic timer kick — wakes `net_poll_task` so smoltcp timers advance without RX traffic |
| **TLS 1.3** | In-kernel via rustls, used by the kernel HTTP client — `cact_tls_connect_ex` / `cact_tls_send` / `cact_tls_recv` / `cact_tls_close`; userspace HTTPS uses the **libc** TLS 1.3 client instead, so session keys never enter the kernel |
| **HTTP/HTTPS** | `cact_http_request` / `cact_http_get` / `cact_http_post` — DNS → TCP → (TLS) → HTTP/1.1 fetch; `Content-Length`, chunked and read-to-close bodies |

**HTTP(S) FFI** (`Cact/net/rust_net_ffi.h`): the kernel can fetch pages without a userspace
process, e.g.

```c
cact_http_resp_t r;
static char buf[65536];
int rc = cact_http_get(&r, "https://example.com/", 0, 0, buf, sizeof(buf));
if (rc == 0) { /* r.status == 200, body at buf + r.body_off, r.body_len bytes */ }
```

HTTPS verifies the certificate chain by default — the webpki root store plus `cact_crypto`'s
RSA (PKCS#1/PSS) and ECDSA (P-256/P-384) signature verification. Skipping verification now
has to be asked for explicitly with `CACT_HTTP_FLAG_INSECURE_TLS` (it logs a warning), for
boards with no trustworthy clock. `cact_tls_connect` uses the same verified path.

**Limits (non-exhaustive):** no **IPv6**; **IPv4 boots unnumbered** (no compile-time default, no in-kernel DHCP) and at most **one NIC + `lo`**; default NIC is **virtio-net** in QEMU; the **Wi-Fi engine is unverified on hardware** (QEMU has no radio); DNS resolver is **A-record only** (one retransmission, no server failover, no TCP fallback for truncated answers); HTTP responses are buffered with a 1 MiB cap; no gzip decompression; one pending inbound connection per listening socket (smoltcp has no SYN backlog); IPv4 fragments are not reassembled.

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

Authoritative ABI headers: [`syscalls.h`](Cact/kernel/core/syscall/syscalls.h) (15 trap numbers) and [`ioctl_abi.h`](Cact/kernel/core/syscall/ioctl_abi.h) (every relay command/protocol struct) — both must stay byte-for-byte in sync with **[CactLib `syscall.h`](https://github.com/QwaYer/CactLibc-x86_32/blob/main/include/syscall.h)**.

Syscall dispatch uses the **`sysenter`/`sysexit`** instruction pair, with an AMD **`syscall`/`sysret`** fast path on CPUs that support it (see [`syscall_entries.asm`](Cact/kernel/core/syscall_entries.asm), `cpu_syscall_mech_*` in [`cpudev.c`](Cact/kernel/cpudev/cpudev.c)); the legacy **`int 0x80`** path has been removed, so SEP (or AMD SYSCALL) is required. Many syscalls take a **`struct syscall_frame*`** (full register snapshot) in the dispatcher — see [`mod.c`](Cact/kernel/core/syscall/mod.c) `_needs_frame()`.

| ABI surface | Service |
|-------|-------|
| **`SYS_*`** 0–14 | Core traps: `open` `close` `read` `write` `ioctl` `poll` `fork` `exec` `exit` `waitpid` `brk` `mmap` `munmap` `mprotect` `sigreturn` |
| **`CACT_FDCTL_*`** 0x3000 | ioctl on any fd — `dup` `dup2` `fcntl` `lseek` `fstat` `fstatx` `ftruncate` `getdents` `fsync` |
| **`CACT_DIRCTL_*`** 0x3100 | ioctl on a directory fd — `openat` `create` `mkdir` `rmdir` `unlink` `link` `symlink` `readlink` `rename`/`renameat` `stat`/`statx` `statfs` `access` `chmod` `chown` `truncate` `mknod` |
| **`CACT_PROCCTL_*`** 0x3200 | ioctl on `/proc/self/ctl` — ids/pgid/sid, `umask` `chdir` `chroot`, the full signal surface (`sigaction` `sigprocmask` `sigsuspend` `sigpending` `kill`/`killpg` `sigwait` `sigqueue` `sigaltstack`), `alarm`/`setitimer`, SysV-style `shm*`, **threads** (`thread_create` `thread_exit` `gettid` `set_tls`), **`futex`**, `sched_yield` |
| **`CACT_SOCKCTL_*`** 0x3300 | ioctl on a socket fd — `bind` `connect` `listen` `accept` `shutdown` `setsockopt` `getsockopt` `getsockname` `getpeername` `sendto` `recvfrom` `sendmsg`/`recvmsg` (SCM_RIGHTS on AF_UNIX), AF_UNIX `bind`/`connect` |
| **`CACT_NETCTL_*`** 0x3400 | ioctl on `/dev/net` — `socket`/`socketpair`, ping, DNS resolve, netcfg, interface list (`iface_count`/`iface_get`), ARP/neighbour table |
| **`CACT_SYSCTL_*`** 0x3500 | ioctl on `/dev/sys` (root) — `mount` `umount` `reboot`, module load/unload, block rescan, hostname, console log level, wall clock |
| **`CACT_PIPECTL_*`** 0x3600 | ioctl on `/dev/pipe` — `pipe` creation |
| **`CACT_CRYPTCTL_*`** 0x3700 | ioctl on `/dev/crypto` — `random`, `hash`, `hmac`, `hkdf`, `aead`, X25519/P-256 key exchange, signature verify, X.509 chain verify |
| **`CACT_MEMFDCTL_*`** 0x3800 | ioctl on `/dev/memfd` — `memfd_create` |
| **`CACT_EVENTFDCTL_*`** 0x3900 | ioctl on `/dev/eventfd` — `eventfd_create` |
| **`CACT_TIMERFDCTL_*`** 0x3A00 | ioctl on `/dev/timerfd` — `timerfd_create` + `SETTIME`/`GETTIME` |
| **`CACT_SIGNALFDCTL_*`** 0x3B00 | ioctl on `/dev/signalfd` — `signalfd_create` + `SETMASK` |
| **`CACT_EPOLLCTL_*`** 0x3C00 | ioctl on `/dev/epoll` — `epoll_create`, `EPOLL_CTL`, `EPOLL_WAIT` |
| **`CACT_TTYCTL_*`** 0x3D00 | ioctl on `/dev/ttyN` — VT activate/state/ctty, console foreground process group |
| **`CACT_PTYCTL_*`** 0x3E00 | ioctl on `/dev/ptmx` — pts number, slave lock |
| **`CACT_WLANCTL_*`** 0x3F00 | ioctl on `/dev/wlan0` — 802.11 scan/BSS list, connect/disconnect, TK/GTK install, EAPOL in/out, status |
| **`CACT_MQCTL_*`** 0x4000 | ioctl on a message-queue fd — POSIX queue init/send/receive/getattr/setattr/notify |
| **DRM uapi** | ioctl on `/dev/dri/cardN` / `renderDN` — the DRM ioctls (`'d'` = 0x64 magic; the `drm_mode_*` band `0xA0`–`0xCE`, vendor commands from `DRM_COMMAND_BASE` 0x40 up) — see **Graphics — DRM / KMS** |

Identity/process info: `/proc/self/info`; cwd: `/proc/self/cwd`; time: `/proc/time` `/proc/wallclock`; `uname`: `/proc/uname` `/proc/<pid>`.

---

## ⚖️ License

**GNU General Public License v3.0** — see [`LICENSE`](LICENSE).

---

<p align="center">
  <strong>Developer:</strong> <a href="https://github.com/QwaYer">QwaYer</a>
  &nbsp;·&nbsp; <strong>libc:</strong> <a href="https://github.com/QwaYer/CactLibc-x86_32">CactLib-x86_32</a>
  &nbsp;·&nbsp; <strong>OS:</strong> <a href="https://github.com/QwaYer/CactOS-x86_32">CactOS-x86_32</a>
</p>
