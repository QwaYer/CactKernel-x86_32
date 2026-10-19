# Roadmap

CactOS is a from-scratch, x86_32, ring-0-minimal OS. This file tracks what is
**not done yet**, so it stays honest: every item is a gap the current code
actually has, not a promise. What already works is described in
[`README.md`](README.md).

## Networking

- **DNS is A-record only.** No AAAA, no resolver failover, and no TCP fallback
  for truncated answers; a lookup sends at most one retransmission.
- **No IPv6** anywhere in the stack, and **no IPv4 fragment reassembly** —
  smoltcp's current configuration drops what it cannot reassemble.
- **HTTP(S)** buffers the whole response with a 1 MiB cap and does **not**
  decode gzip.
- **Listening sockets** hold a single pending inbound connection: smoltcp has no
  SYN backlog yet, so a second concurrent connect is refused.

## Interrupts & drivers

- MSI-X is the preferred device interrupt, with a single **MSI** message as the
  fallback (`msidev_register()`); both paths need coverage across the driver set.
- **GPU / KMS**: the DRM uapi and a virtio-gpu driver exist as a sibling repo;
  the VT/PTY layer has no automated self-test. There is **no Intel GPU driver
  in 2.0.0** — the i915 port is heavily 64-bit-bound, so it is deferred to
  3.0.0 (see [GPU / Intel (i915)](#gpu--intel-i915) for the kernel gaps it
  exposes).

## GPU / Intel (i915)

Assessed 2026-09-25. There is no Intel driver and no groundwork for one; what is
missing is **kernel facilities**, not DRM plumbing. The reusable half is real
and runtime-verified: PCI enumeration with 64-bit BAR sizing over ECAM or legacy
config space, MSI/MSI-X with a 192-vector pool, `vmm_map()`/`vmm_get_phys()` for
driver-owned page tables, an uncached MMIO path, ACPICA's AML interpreter, and
the in-tree DRM/KMS core (GEM, dumb buffers, mmap, atomic modeset, planes,
cursors, syncobj, PRIME).

Ordered by how hard each gap blocks a port:

- **No order-N / contiguous physical allocator.** `kalloc()` returns a single
  4 KiB frame and the heap is a 16 MiB fragmented free list (`HEAP_SIZE` in
  `kernel/memory/memory.h`); `kalloc` is exported to modules (ksym, 2.0.0) but
  still has no multi-frame/contiguous variant. Scanout and a GTT both assume
  contiguous, aligned backing.
- **GEM storage is scattered but no longer size-capped.** A GEM is a memfd:
  individually allocated frames whose pointer array now grows from the heap, so
  objects are bounded by free RAM rather than the old 4 MiB ceiling (the fixed
  `MEMFD_MAX_PAGES = 1024` in `rust_mm/src/process/memfd.rs`). Still nothing is
  exported to walk an object's frames to collect their physical addresses, which
  is exactly what a GTT needs.
- **Only `DRM_FORMAT_MOD_LINEAR` is accepted.** Every Intel tiling modifier is
  refused in `kms/src/framebuffer.rs`, so tiled scanout cannot be described even
  though the constants are vendored in `drm/uapi/drm_fourcc.h`.
- **No general write-combining mapping.** `vmm_map()` forces `PCD|PWT` for any
  physical address ≥ the runtime `pmm::ram_end()` (`rust_mm/src/vmm/paging.rs`),
  so PAT entry 4 (WC, programmed in `memory/pat.c`) is unreachable for a BAR;
  the only WC helper is `pat_enable_wc_for_framebuffer()`. MTRR is not used for
  this — there is no range allocator.
- **No runtime firmware loader.** `request_firmware` (ksym) can stage a blob
  that ships in the build-time cctkfs table, but there is no on-demand loader or
  workqueue, so GuC/HuC/DMC blobs have no practical path into kernel memory.
- **No ACPI OpRegion, `_DSM` or VBT reader.** The AML interpreter is linked, but
  there is no address-space handler, no video-BIOS-table parsing, and no ACPI
  entry point exported to modules.
- **No I2C/DDC/GMBUS and no EDID parser.** EDID reaches the DRM core only as a
  blob a driver supplies (`drm_connector_set_edid`); nothing can discover a
  panel or its modes itself.
- **No workqueue / tasklet / threaded IRQ.** ISRs are top-half `void(void)`
  handlers (`msidev.h`); the only deferral mechanisms are `sched_sleep_ticks()`
  and the PCI deferred-probe list, and there is no kernel timer callback API.
- **No dma-buf / dma-fence.** PRIME is memfd-based and syncobj is software-only,
  so there is no cross-device or imported-buffer synchronisation.
- **No sysfs/kobject, runtime PM (D0/D3), forcewake/clock gating, PSR, or
  IOMMU/DMAR.** Only system-wide ACPI S5 (poweroff) exists.
- **No BAR (re)assignment or resource tree.** Enumeration sizes BARs correctly
  (64-bit included) but trusts whatever firmware programmed; there is no
  allocator to move one.
- **The module ABI is narrower than the full DRM core.** `kernel/elf/ksym.c`
  exports the generic KMS/GEM/connector API but no property/blob or framebuffer
  creation, so the property/framebuffer surface a full render driver needs is
  still missing.
- **RAM above 4 GiB is unreachable.** The PMM only manages frames below 4 GiB
  (`TOTAL_PAGES = 1024*1024`) and stops at the runtime PCI hole (`pmm::ram_end()`,
  default `0xC0000000`); a 32-bit non-PAE kernel cannot map the rest.

The order that unblocks the most:

1. **Order-N / contiguous allocator**, plus an exported "physical address of
   page N of a GEM object". Nothing GPU-side moves without this.
2. **WC mapping as an API** (`vmm_map_wc` / `set_memory_wc`) and dropping the
   forced UC above the PCI hole.
3. **Firmware loader** and **workqueue** — needed by i915 and by other drivers.
4. **ACPI OpRegion + VBT** and **GMBUS/DDC + an EDID parser** — the display half
   cannot probe a panel without them.
5. Then the driver itself, **in-tree**, because it needs the
   property/framebuffer/ACPI/PAT surface the module ABI does not export.

### Dropped from 2.0.0 (2026-10-16) — the out-of-tree Intel-GPU (i915) module

An out-of-tree port (`Intel-GPU-for-Cact-x86_32`, `i915.cctk`, from Linux i915
7.3-rc4) had reached stages 1–4 — bind + EDID over GMBUS/DDC, CDCLK/WRPLL,
pipe/plane scanout with console handoff, and vblank/flip/hardware cursor.  It is
**no longer part of the 2.0.0 build**: `Intel-GPU` was removed from
`CactBridge-x86/drivers.list` (the single list the integrator also reads), so the
module is no longer built, staged or packed into `cctkfs.img`.  The port is
heavily 64-bit-bound, so it belongs to the 3.0.0 x86_64 line, not the last 32-bit
release; the source repo stays out-of-tree for that work.  The kernel-side
facilities it needed remain on the gap list above, and `ksym.c` keeps its
`drm_gem_page_phys()` / `drm_gem_page_count()` exports (GGTT scanout backing) and
the `__divdi3` / `__moddi3` / `__udivdi3` helpers the WRPLL calculator used.

## CPU topology: SMP, SMT (Hyper-Threading) & core power

**Closed in 2.0.0 (P1.2).** The kernel decodes CPUID leaf `0xB`/`0x1F`
(falling back to leaf `4` + leaf `1` HTT, then MADT order) into
`package_id`/`core_id`/`smt_sibling_mask`; `/proc/cpuinfo` reports `core id`,
`physical id`, `siblings` and `cpu cores`; placement prefers a fresh physical
core over a sibling of a busy one (`balance.rs`/`decision.rs`); deep C-states
(C3/C6) are gated on *all* siblings idle (`cap_idle_depth`); and a whole
physical core is parked / offlined as one unit (`core_mask`).  The host unit
tests cover the topology decoder and the placement policy; C6 is confirmed on
hardware that advertises `_CST`.  **S3 suspend/resume is removed in 2.0.0
(poweroff and reboot remain).**

## Storage & filesystems

- The page cache and optional **swap** are in place; a swap partition that
  cannot be used today only logs a warning.
- Cross-directory rename works for files everywhere and for **directories** on
  every filesystem that implements the `vfs_ops_t.rename2` op — **tmpfs, ext4
  and fat32** do.  The fat32 path is host-tested (`FAT32-for-Cact-x86_32/
  test/`); the ext4 path is build-verified only because the check image carries
  no ext4 volume.  `fsync` (`CACT_FDCTL_FSYNC`) flushes the file's inode page
  cache to its backing store.  `statx` timestamps come from the wall clock (the
  VFS does not track per-inode times yet).  AF_UNIX `setsockopt`/`getsockopt`
  and the abstract namespace are supported.

## ABI & userspace

- The **15-trap** syscall ABI is considered final. New subsystems join as ioctl
  ranges in `ioctl_abi.h` (fd, dir, proc, socket, net, sys, pipe, crypto, tty,
  pty), never as new traps.
- All repos build with **Meson + Ninja**; the toolchain is `clang -m32` against
  the workspace's own libc.

## Versioning & releases

CactOS uses **SemVer**, but a release is a self-describing triple
`(version, arch, module-ABI)`:

- **`MAJOR` = architecture generation.** `2.x` is the final **i686** line;
  `3.x` is the **x86_64** line. The major number only moves on an arch
  generation / ABI-epoch break, never for features, so `2.0.0` is terminal and
  the next release is `3.0.0` (there is no `2.1`).
- **`MINOR`/`PATCH`** are the usual feature / fix levels; pre-releases are
  `-rc.N` (`v2.0.0-rc.1`).
- **Build metadata carries the machine identity**: `<version>+abi.<vermagic>.i686`
  (e.g. `2.0.0+abi.0x9217b2c9.i686`). `<vermagic>` is the module ABI fingerprint
  the kernel enforces (see `module-signing`), so the kernel banner, `/proc/version`
  and every `.cctk` agree on one value — no more "2.0.0, but which ksym set?".
  Metadata never appears in a git tag.
- **Tags** are the plain version: `v2.0.0` (and `v2.0.0-rc.N` before it).

The `meson.build` footer, the boot banner and `/proc/version` print the full
metadata form; `uname -r` reports the plain `MAJOR.MINOR.PATCH`.
