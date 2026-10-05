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
  the VT/PTY layer has no automated self-test. The Intel
  display half now exists as the out-of-tree module
  `Intel-GPU-for-Cact-x86_32` (stage 1: bind + EDID → `/dev/dri`); see
  [GPU / Intel (i915)](#gpu--intel-i915) for what a full port still needs.

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
- **GEM storage is scattered and capped.** A GEM is a memfd: individually
  allocated frames with `MEMFD_MAX_PAGES = 1024` (`rust_mm/src/process/memfd.rs`)
  — a 4 MiB object ceiling — and nothing exported to walk an object's frames to
  collect their physical addresses, which is exactly what a GTT needs.
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
  exports the generic KMS/GEM/connector API (enough for the out-of-tree display
  half below) but no property/blob or framebuffer creation, so the
  property/framebuffer surface a full render driver needs is still missing.
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

### Update 2026-10-01 — display half started as an out-of-tree `.cctk`

The **display** half turned out to be reachable out-of-tree after all: `ksym.c`
already exports the whole generic DRM/KMS API (`drm_dev_create`, KMS object
init, GEM, connector/EDID helpers) and the loader matches PCI by
vendor + device-list, which is what a display driver needs.  Only the *full*
i915 (property/blob creation, render engine) still needs the in-tree surface.

Started `Intel-GPU-for-Cact-x86_32/` (`i915.cctk`, ported from Linux i915
7.3-rc4; CFL is DISPLAY_VER **9**, not 10): stage 1 = bind + power wells + EDID
over GMBUS/DDC or DP AUX → `/dev/dri` with real connector modes.  `set_config`
answers `EOPNOTSUPP` — nothing is scanned out yet.

Kernel side: **gap 1 is half closed** — `drm_gem_page_phys()` /
`drm_gem_page_count()` now export the physical backing of a GEM object (via
`vmm_get_phys`), which is what GGTT scanout needs.  Still absent for a real
modeset: the CDCLK/DPLL/pipe/plane programming (stage 3) and, for tiling and
plane/GTT-mapped scanout, the WC mapping API (gap 2).

### Update 2026-10-02 — stage 3 scanout + console handoff

The driver now **drives the display**: on load it finds the transcoder already
serving the connected DDI, maps the kernel's boot framebuffer into the GGTT and
programs pipe/plane, transcoder timings, `TRANS_DDI_FUNC_CTL`, watermarks and
the DDI encoder, so the console keeps working through the driver's own modeset.
`set_config()` programs a client framebuffer the same way.  CDCLK/WRPLL are
still inherited from firmware — programming them is the remaining stage-3 work.

Kernel side: `ksym.c` exports `__divdi3`/`__moddi3`/`__udivdi3` (the driver's
WRPLL calculator does 64-bit division).  Two driver-side fixes came out of this:
the module is registered in `CactBridge build.py`'s `DRIVERS`
(a missing entry meant it was silently never rebuilt), and `intel_probe_ports()`
now keeps the DDI index instead of compacting the port array (it mislabelled
every sink as DDI-A, which would have programmed the wrong port).

### Update 2026-10-02 — stage 3 clocks + stage 4 (vblank / flip / cursor)

Stage 3 is complete: the driver programs **CDCLK** (kept or changed via the
Wa #1183 DIVMUX sequence + PCODE) and the **HDMI WRPLL** from scratch
(`skl_ddi_calculate_wrpll` ported, `DPLL1` lock), routes the DPLL to the port
and transcoder, and takes over pipe/encoder/plane.  Because the boot
framebuffer's address is an *aperture* address (GMADR + 0), not physical
memory, the console handoff keeps the firmware's plane buffer/mapping rather
than re-mapping pages.

Stage 4 is implemented: a real **vblank interrupt** (GPU MSI handler →
`GEN8_DE_PIPE_IIR` ack → `drm_crtc_handle_vblank`), an MMIO **page flip**
(`drm_gem_page_phys` → GGTT → `PLANE_SURF`), and the **SKL hardware cursor**
(`CUR_CTL/POS/BASE` + `CUR_WM`/`CUR_BUF_CFG`).  Kernel side gained
`__divdi3`/`__moddi3`/`__udivdi3` in `ksym` (the WRPLL calculator does 64-bit
division).

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
