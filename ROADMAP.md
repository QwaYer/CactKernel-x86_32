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
  the VT/PTY layer has a self-test (`devtest` in CactUserBins). The Intel
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
  `kernel/memory/memory.h`); `kalloc` is not even in the ksym table. Scanout and
  a GTT both assume contiguous, aligned backing.
- **GEM storage is scattered and capped.** A GEM is a memfd: individually
  allocated frames with `MEMFD_MAX_PAGES = 1024` (`rust_mm/src/process/memfd.rs`)
  — a 4 MiB object ceiling — and nothing exported to walk an object's frames to
  collect their physical addresses, which is exactly what a GTT needs.
- **Only `DRM_FORMAT_MOD_LINEAR` is accepted.** Every Intel tiling modifier is
  refused in `kms/src/framebuffer.rs`, so tiled scanout cannot be described even
  though the constants are vendored in `drm/uapi/drm_fourcc.h`.
- **No general write-combining mapping.** `vmm_map()` forces `PCD|PWT` for any
  physical address ≥ `PCI_HOLE_START` (`rust_mm/src/vmm/paging.rs`), so PAT
  entry 4 (WC, programmed in `memory/pat.c`) is unreachable for a BAR; the only
  WC helper is `pat_enable_wc_for_framebuffer()`. MTRR is save/restore across S3
  only — there is no range allocator.
- **No firmware loader.** No `request_firmware` equivalent exists;
  `initfs_modblob_get()` is a build-time blob table. GuC/HuC/DMC have no path
  into kernel memory at all.
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
  IOMMU/DMAR.** Only system-wide ACPI S3/S5 exists.
- **No BAR (re)assignment or resource tree.** Enumeration sizes BARs correctly
  (64-bit included) but trusts whatever firmware programmed; there is no
  allocator to move one.
- **The module ABI is narrower than the DRM core.** `kernel/elf/ksym.c` exports
  no property/blob creation, no framebuffer creation and no ACPI/PAT/MTRR
  helper, so i915 has to be **in-tree**, not an out-of-tree `.cctk`.
- **Only ~3 GiB of RAM is managed.** The PMM stops at `PCI_HOLE_START`
  (`0xC0000000`); memory above the firmware hole is ignored.

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

Assessed 2026-09-30. SMP works and the energy governor already parks, offlines
and re-wakes individual CPUs (`smp_cpu_offline` / `smp_cpu_online_sipi` in
`kernel/proc/sched/src/smp.rs`, gated by `energy_core_offline_enable()`), but the
CPU map is **flat**: one entry per MADT LAPIC id (`energy.rs`,
`MAX_CORES = MAX_CPUS = 64`) and nothing records that two of those entries are
the two **threads of one physical core**. SMT siblings already come up — MADT
enumeration counts every enabled LAPIC entry — they are simply indistinguishable
from real cores, so a 4-core / 8-thread CPU is reported and scheduled as 8
independent cores.

What is missing to make the machine's own topology real (4 cores → 8 CPUs, each
logical processor tied to its core and switchable):

- **No topology decode.** `cpu_has_htt()` reads CPUID.01H:EDX[28]
  (`kernel/cpudev/cpudev.c`) but **nothing consumes it**, and neither leaf `4`
  (cores/package) nor leaf `0xB`/`0x1F` (package/core/SMT bit widths) is
  decoded — so there is no `core_id`, `package_id` or sibling mask anywhere.
- **Core vs thread is absent from the model.** `EnergyCore` carries only
  `lapic_id`/`role`/`cstate`/`online`; there is no parent core or sibling set to
  reason about, and the "core" wording is really a logical CPU.
- **Power is per CPU only.** `energy_core_set_cstate` lets a thread enter C3/C6
  while its sibling runs, and there is no "all threads of a core idle → the
  core may go deep" rule; the offline path also acts on one CPU, not on a core
  as a unit. Powering a core off means parking **both** threads, not one.
- **Placement is sibling-blind.** `balance.rs` / `decision.rs` scan
  `1..MAX_CORES` and treat every entry as an independent core, so two runnable
  tasks can land on the two threads of one core while another physical core
  idles.
- **S3 loses the workers.** `smp_init()` runs only from boot
  (`kernel/core/kernel.c`); after an S3 resume no AP (let alone a sibling set)
  is re-woken — the existing gap, and the topology work has to close it.
- **Reporting.** `/proc/cpuinfo` (`fs/vfs/procfs/procfs_std.c`) prints
  `processor`/`apicid`/`role`/`cstate`/`online`/`idle` but no `core id`,
  `physical id`, `siblings` or `cpu cores`, so userspace cannot see the
  topology; `sysinfo` has no "N cores / M threads" line.

### Phases

1. **Topology.** Decode CPUID leaf `0xB`/`0x1F` into `package_id` / `core_id` /
   `smt_sibling_mask`, falling back to leaf `4` + leaf `1` HTT and then to MADT
   ordering; expose it through `energy.h` and add `core id`, `physical id`,
   `siblings` and `cpu cores` to `/proc/cpuinfo`.
2. **Core-level power.** Deep C-states only when *every* sibling is idle; a
   core-offline that parks and INITs all threads of the core as one unit
   (reuse the park protocol already in `smp_cpu_offline`).
3. **Sibling-aware placement.** In `balance.rs` / `decision.rs`, prefer a
   physical core with no busy thread over an SMT sibling of a busy one.
4. **S3.** Re-wake all logical CPUs on resume (closes the gap above).
5. **Self-test** in `CactUserBins`: with `-smp 4,threads=2`, print present
   cores/threads, check the sibling masks, offline one thread and confirm its
   sibling keeps running.

**Acceptance:** with `-smp 4,threads=2` the boot `smp` line reports 7 worker(s)
online (8 logical CPUs) and `/proc/cpuinfo` groups them under 4 `physical id`s
with correct `siblings`; two CPU-bound threads are placed on different physical
cores before any core is shared; offlining one thread leaves its sibling running
and offlining both powers the core down; after S3 all 8 are online again.

## Storage & filesystems

- The page cache and optional **swap** are in place; a swap partition that
  cannot be used today only logs a warning.

## ABI & userspace

- The **15-trap** syscall ABI is considered final. New subsystems join as ioctl
  ranges in `ioctl_abi.h` (fd, dir, proc, socket, net, sys, pipe, crypto, tty,
  pty), never as new traps.
- All repos build with **Meson + Ninja**; the toolchain is `clang -m32` against
  the workspace's own libc.
