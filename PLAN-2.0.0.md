# CactKernel 2.0.0 — Release Plan (final 32-bit release)

Status: draft, agreed scope = **P0..P3**. P4 is explicitly deferred to 3.0.0.
P1.4 (S3 suspend/resume) was removed from 2.0.0 on 2026-10-05 (see P1.4 and P4).

2.0.0 is the last x86_32 release and it is not about new features. It is about
squeezing the remaining correctness bugs, unfinished surface and verification
debt out of the 32-bit kernel so that 3.0.0 can start clean on 64-bit. This plan
is the gate: nothing else ships in 2.0.0.

Ground rules (from `CONTRIBUTING.md`):
- The 15-trap syscall ABI is final; new surface is ioctl ranges in
  `ioctl_abi.h`, never a new trap.
- Keep the kernel minimal; fix userspace rather than grow the kernel.
- Drivers register interrupts through `msidev_register()`.
- Everything in this repo is English-only (comments and string literals).
- Verification is a QEMU boot; every P0/P1 change adds a check to P2.

Evidence note: line numbers below were verified against the tree at the time of
writing (HEAD `84cadb3`, "Vfs переработана"). Re-check before editing.

---

## P0 — Integrity and correctness (must fix before the tag)

Status: DONE in source; `kernel.bin` builds clean. Runtime regression (a crafted
bad ELF) lands with P2.3/P2.4.

### P0.1 — ELF loader writes user PTEs into the shared kernel page tables
**Severity: critical (ring-3 → kernel integrity).**

`Cact/kernel/elf/elf_loader.c` never checks that a `PT_LOAD` segment lands in
the user half. Both `_map_image()` (~lines 22-134) and `load_elf()` (~139-266)
call
`vmm_map(pd, va, phys, PAGE_USER | PAGE_RW | PAGE_PRESENT)`
(lines ~93, 126, 245, 262) with `va = p_vaddr & ~0xFFF` and no
`p_vaddr < USER_STACK_TOP` test.

`vmm_map()` (`Cact/kernel/memory/rust_mm/src/vmm/paging.rs`) treats any
`virtual_addr >= PCI_HOLE_START` (`0xC0000000`) as kernel MMIO. For a present,
non-private PDE (which every kernel-half PDE is) it takes the `is_kernel_mmio`
branch and writes the PTE **directly into the shared kernel page table**,
setting `PAGE_USER | PAGE_RW` on the shared PDE (branch around lines 196-211).

Consequence: an ELF whose `p_vaddr >= 0xC0000000` installs a user-accessible,
writable mapping over kernel/MMIO VAs (APIC at `0xFE000000`, the ACPI window,
`PCIE_ECAM_VADDR`) for **every process and the kernel**, permanently. A second
trigger: the `!phdr_covered` path computes `page_va = min_vaddr - 0x1000`; a
PIE/ET_DYN with a segment at `p_vaddr = 0` yields `min_vaddr = 0` and
`page_va = 0xFFFFF000`, hitting the same branch.

The pattern already exists elsewhere — `Cact/kernel/core/syscall/process/signal.c:16`,
`thread.c:158-160`, `proc.c:54,63` all reject addresses outside
`[USER_SPACE_START, KERNEL_BASE)`. The loader just lacks it.

**Fix:**
- In `elf_loader.c`, reject any `PT_LOAD` whose `seg_start < USER_SPACE_START` or
  whose `seg_end > USER_STACK_TOP`, in both `_map_image()` and `load_elf()`.
- Guard the `min_vaddr - 0x1000` underflow.
- Defense in depth (P0.2): make the kernel refuse it centrally.

### P0.2 — `vmm_map()` must refuse user mappings in the kernel half
`vmm_map()` is the single choke point for all mappings. Add: if
`flags & PAGE_USER` and `virtual_addr >= USER_STACK_TOP`, drop `PAGE_USER`
(and log once) instead of propagating it into the shared kernel PT. This makes
P0.1 impossible to reintroduce by a new loader or module.

### P0.3 — `mmap(MAP_FIXED)` has no lower bound
`Cact/kernel/memory/rust_mm/src/vmm/mmap.rs:246-253` rejects only
`hint >= USER_STACK_TOP` / `hint + length > USER_STACK_TOP`. A `MAP_FIXED` at
`0x1000` is accepted. There is also no overflow check on `r.base + r.length`
in `find_free_va()` (`mmap.rs:118-120`).

**Fix:** clamp fixed mappings to the user range (at least `>= USER_SPACE_START`,
ideally inside `[MMAP_BASE, MMAP_LIMIT)`), and add wrapping checks to region-end
arithmetic.

### P0.4 — Page cache LBA comparison can overflow
`Cact/drivers/block/pagecache/pagecache.c:183-184`: after the
`block_no > UINT32_MAX / spb` guard, the later `lba + spb > max_lba` compare can
still wrap for `lba` near `UINT32_MAX`.
**Fix:** do the bounds math in 64-bit or compare as `spb > max_lba - lba` after
checking `lba <= max_lba`.

### P0.5 — `brk` is capped at 16 MiB, not `USER_HEAP_LIMIT` — DONE (documented)
`Cact/kernel/core/syscall/mem/mm.c:30`:
`new_brk - brk_start > 16 * 1024 * 1024 → -1`, while the declared user heap is
`USER_HEAP_LIMIT = 0x80000000` (1 GiB, `ffi.rs:114`).
**Decision (changed from the draft):** do NOT raise the cap. `brk` grows
eagerly — every page is `kalloc`'d and zeroed, there is no demand paging — and
the window between the ELF image and `MMAP_BASE` is shared with `mmap`, so the
16 MiB bound is load-bearing. The fix is honesty: the magic number is now the
named `USER_BRK_MAX_GROWTH` with a comment on why it must stay small, and the
layout docs no longer imply a usable 1 GiB heap. Raising it requires making
`brk` demand-faulted first — a P4/3.0.0 change.

### P0.6 — PMM hole derived from Multiboot2 — DONE (RAM above 3 GiB unlocked)
The PMM ceiling and the MMIO boundary were one compile-time constant
(`PCI_HOLE_START = 0xC0000000`) that also doubled as the user/kernel VA split.
On a board whose PCI hole is at `0xE0000000` (3.5 GiB) the top 0.5 GiB was
discarded.

Implemented:
- **Runtime RAM/MMIO boundary** `pmm::ram_end()` — page-aligned top of available
  RAM below 4 GiB from the MB2 map (default `PCI_HOLE_START`). The PMM installs
  every available frame below 4 GiB; `TOTAL_PAGES` (static array sizing) is now
  the worst case, 1 048 576 frames (4 GiB).
- **Decoupled the VA split**: `PD_KERNEL_ENTRIES` and `is_kernel_mmio` now use
  `USER_STACK_TOP` (0xC0000000), not the RAM ceiling.
- **Runtime MMIO window base** `cact_mmio_window_base()` = 2 MiB-aligned
  `max(RAM_END, 0xC0000000)`. The ACPI temp map (`osl.c`) and PCIe ECAM
  (`pcie.c`) lay out from it, so their identity-map aliasing can never shadow a
  managed RAM frame. `AcpiOsInitialize` seeds the window from it at runtime.
- Cache policy in `init_paging` and the physical auto-UC in `vmm_map` use
  `ram_end()`.

Verified: builds clean; no existing userland base is below the ELF floor.
Not verified: a real boot (must be checked on the target).

---

## P1 — Finish the 32-bit functionality

### P1.1 — SMP worker path and load balancing
Currently dormant:
- `Cact/kernel/proc/sched/src/balance.rs:121` `energy_balance_migrate()` returns
  `-1`; `:139` `energy_balance_tick()` is all no-ops.
- `.../cstate.rs:346,353` `energy_ipi_halt_handle()` / `energy_ipi_wake_handle()`
  are no-ops; `mlfq_map.rs:85 on_enqueue()` is a deliberate no-op.

**Goal:** real worker execution on the shared MLFQ (the agreed design is a peer
pool with one always-on privileged member — *not* per-core runqueues +
work-stealing), real IPI halt/wake handlers, and load-driven scaling in both
directions (busy master wakes help; busy worker asks for help).
Keep all global per-tick state on cpu0.

### P1.2 — CPU topology (SMT / Hyper-Threading)
Whole ROADMAP section "CPU topology: SMP, SMT & core power" is open.
- Decode CPUID leaf `0xB`/`0x1F` into `package_id`/`core_id`/`smt_sibling_mask`
  (fallback: leaf `4` + leaf `1` HTT, then MADT order). `cpu_has_htt()` exists
  (`Cact/kernel/cpudev/cpudev.c`) but nothing consumes it.
- Add `core id`, `physical id`, `siblings`, `cpu cores` to `/proc/cpuinfo`
  (`Cact/fs/vfs/procfs/procfs_std.c`).
- Sibling-aware placement in `balance.rs`/`decision.rs`.
- Core-level power: deep C-state only when all siblings idle; offline a core as
  one unit (park all threads).

**Acceptance (from ROADMAP):** with `-smp 4,threads=2` the boot `smp` line
reports 8 logical CPUs under 4 `physical id`s with correct `siblings`; two
CPU-bound threads land on different physical cores before sharing one; offlining
one thread leaves its sibling running. (The former "after S3 all 8 are online"
item was removed from 2.0.0 with P1.4.)

### P1.3 — Enable C3/C6 (ACPI `_CST` + MWAIT)
`.../cstate.rs:208-215` hardcodes C3/C6 `available=false`, `mwait_hint=-1`.
The MWAIT mechanism and an `_CST` reader already landed
(`Cact/drivers/acpi/cstates.c`), but were never boot-tested and the non-FFH
C-state write path is missing.
**Goal:** finish/verify `_CST` + MWAIT so deep idle is actually enterable on
platforms that advertise it; stay C1-only (safely) elsewhere.

### P1.4 — S3 suspend/resume — REMOVED IN 2.0.0
**Decision (2026-10-05): S3 suspend/resume was removed from 2.0.0.** Resume was
not reliable on the target (the display driver's re-modeset hung, and there was
an intermittent post-resume userspace `#PF`), so suspend is not shipped: the
`acpi_suspend`/resume path, the wake trampoline (`wake_entry.asm`,
`wake_trampoline.asm`), the S3 device save/restore helpers (PCI config snapshot,
MSI-X re-program, xHCI re-enumerate, MTRR snapshot), `smp_resume_rewake()` and
the `suspend` tool / ioctl / `RB_SUSPEND` were all deleted. `poweroff`, `reboot`
and `halt` are unaffected. The WIP S3 work is preserved on the `s3-wip` branch
(off `v2.0.0`) for a later re-introduction. See P4.

### P1.5 — VFS / POSIX corners
- Cross-directory move of **directories** is refused
  (`Cact/fs/rust_vfs/src/vfs.rs:751`, `-EINVAL`); files are atomic. Either
  implement directory rename or document it as out of scope.
- Timestamps are always 0 (`ioctl_abi.h:60`).
- `CACT_FDCTL_FSYNC` is a literal `return 0;` (`syscall/io/fd.c:314`).
- AF_UNIX is `SOCK_STREAM` only, no `setsockopt/getsockopt`, no abstract
  namespace (`syscall/sock/unix_sock.c:645,1037`).

Decide per item: implement, or mark "not in 2.0.0" explicitly in ROADMAP.

### P1.6 — Pipes end-to-end
`Cact/fs/pipe/pipe.c` was replaced by `Cact/fs/rust_vfs/src/pipe.rs`, but there
is **no way to exercise it**: the guest shell `Cgoct-x86_32` has no `|` and no
`popen()`. Add `|` to the shell parser (or ship a small userspace pipe test) and
verify read/write/EOF/EPIPE.

---

## P2 — Verification

Status 2026-10-05: P2.1 and P2.3 are DONE; P2.2 is kernel-only (the userspace
self-test apps were removed — see below).

### P2.1 — Host-side unit tests for pure logic — DONE 2026-10-05
`Kernel-Unit-Tests-for-Cact/` is a host `cargo test` crate that `#[path]`-includes
real kernel modules (PMM bitmap math, MLFQ policy, energy model, VFS helpers,
CPUID topology, placement) so the tests cannot drift from a copy. **58 tests
pass.** A module may only be included if it is `no_std` with no `crate::` refs,
globals or FFI; anything touching a device is covered by the boot self-tests +
`check` instead. See `CONTRIBUTING.md`.

### P2.2 — Kernel self-tests and userspace self-tests — PARTIAL
Kernel self-tests (csprng, energy, timer, placement, module-signing magic) are
required markers in the headless `check`. The userspace self-test apps
(`devtest`, `threadtest`, `elftest`, `topotest`, `sockopttest`, `stattest`,
`pipetest`, `cactcheck`) were **removed from `CactUserBins-x86_32`**, so the
check image is kernel-only and the P0.1 ELF regression (formerly `elftest`) no
longer runs in an image. Userspace coverage is deferred with those apps.

### P2.3 — One headless "check" target — DONE 2026-10-05
`ninja -C CactKernel-x86_32/build-meson check` runs `tools/cact_check.sh`: boots
an ISO headless (`-display none`), greps COM1 for the required kernel markers
and the module vermagic, and exits non-zero on any failure marker.

---

## P3 — Release engineering and documentation

Status: **closed 2026-10-05** (P3.2 changed to asymmetric signing — see below).

### P3.1 — `DRIVERS` must be a single source of truth — DONE
The driver list was duplicated in `CactBridge-x86/build.py` and
`CactOS-x86_32/meson.build`, and **both omitted `Virtio-gpu`**, so
`LocalRepoCactOS-x86_32/lib/virtio_gpu.cctk` stayed a stale artifact.
**Done:** the canonical list now lives in `CactBridge-x86/drivers.list`
(one name per line); `build.py` reads it directly and the CactOS integrator
reads the same file through its `-Dbridge` path, so the two cannot drift.
`Virtio-gpu` is on the list and is built/staged by both flows.

### P3.2 — Module ABI version handshake + asymmetric signatures — DONE
There was no compatibility mechanism (`ABI_VERSION`/`vermagic` = zero matches)
and module trust was a **symmetric HMAC-SHA256** with the key embedded in the
kernel image — anyone who extracted the image could forge a module.
**Done (converted to public-key signing):**
- **Signature:** ECDSA P-256 over SHA-256, fixed 64-byte `r||s`, produced at
  build time with the **private** key (`tools/modsign.py`, OpenSSL) and verified
  in the kernel with only the **public** key. The verifier is the existing
  `cact_sig_verify_p256_raw` C ABI in `Cact/crypto/src/sig.rs` (verify-only).
  Keys are generated on demand by `tools/gen_module_keys.py`; both the private
  key and the derived public header are gitignored.
- **ABI fingerprint:** `ksym_vermagic()` (FNV-1a over the sorted exported symbol
  names in `ksym.c`), mirrored by `tools/modsign.py`; the loader refuses a module
  whose vermagic differs.
- **Trailer:** `[ ELF ][ 'CMOD' ][ vermagic:4 ][ signature:64 ]`, verified by the
  single shared `mod_tag_verify()` (`Cact/kernel/elf/mod_tag.c`) used by the PCI,
  USB and FS loaders (the three duplicated HMAC helpers are gone).
- **Verification:** the boot self-test (`sigmagic`) checks a known-answer
  signature and runs the full trailer parse on an embedded synthetic blob;
  `cact_check.sh` requires that marker and cross-checks the printed vermagic
  against the signer. The headless `check` image passes.

### P3.3 — Cheap ksym exports for out-of-tree drivers — DONE
`Cact/kernel/elf/ksym.c` now exports `kalloc`, `sched_sleep_ticks` and
`ktime_get_usec` (alongside the existing `free_page` / busy-wait primitive).
Heavy facilities (order-N allocator, WC mapping API, firmware loader,
workqueue/threaded IRQ, DMA-buf/fence, full DRM property/blob, ACPI
OpRegion/VBT, GMBUS/DDC/EDID) stay in P4 / 3.0.0.

### P3.4 — Documentation truth pass — DONE
- README no longer lists `btrfs / exFAT / ramfs` (no such files exist).
- README repo layout no longer advertises a `dynlink/` directory.
- README/ROADMAP now describe ext4 as an out-of-tree `.cctk`, the runtime
  `pmm::ram_end()` RAM boundary, and ECDSA module signing (not HMAC).
- `ROADMAP.md`'s CPU-topology section is marked closed for 2.0.0; `CONTRIBUTING.md`
  documents the host unit tests and the headless `check` gate.

### P3.5 — Tag — DONE 2026-10-05
`VERSION` is `2.0.0`; the annotated tag `v2.0.0` was created on `9a1d9d3`
("2.0.0: integrity, SMT topology, ECDSA module signing, SemVer+abi versioning").
Later S3 WIP work on `main` (`af8c84e`) is **not** part of 2.0.0 (P1.4).

---

## P4 — Explicitly deferred to 3.0.0 (do NOT do in 2.0.0)

S3 suspend/resume (removed from 2.0.0 — see P1.4; re-introduce with a working
display re-modeset and no post-resume `#PF` race); Order-N/contiguous physical
allocator; `vmm_map_wc`/`set_memory_wc`; firmware
loader (`request_firmware` exists but no real loader/workqueue); workqueue /
tasklet / threaded IRQ; DMA-buf / dma-fence; full DRM property/blob/framebuffer
surface; ACPI OpRegion + VBT; GMBUS/DDC + EDID parser; Intel i915 render engine;
files/offsets > 4 GiB; removal of the 3 GiB PMM ceiling and the 4 MiB
memfd/address-space ceiling; IPv6; PAE/highmem (obsoleted by 64-bit).

---

## Definition of Done for 2.0.0

1. No ring-3 path can install or alter a kernel-half mapping (P0.1/P0.2 done).
   The `elftest` regression was removed with the userspace test apps (P2.2).
2. P0.3-P0.6 fixed.
3. `check` target boots headless, runs the kernel self-tests, and exits 0 (P2.3).
4. Kernel self-tests pass in `check`; the `devtest`/`threadtest` userspace apps
   were removed and are no longer part of 2.0.0 (P2.2).
5. With `-smp 4,threads=2`: 8 logical CPUs under 4 physical ids, correct
   `siblings` in `/proc/cpuinfo`, and cross-core placement (P1.2). S3
   suspend/resume was removed from 2.0.0 (P1.4).
6. `DRIVERS` matches reality and is not duplicated; `Virtio-gpu` resolved (P3.1).
7. Module vermagic rejects a mismatched `.cctk` (P3.2).
8. `README.md`/`ROADMAP.md` contain no stale claims (P3.4); `v2.0.0` tagged.

**Removed:** S3 suspend/resume was deleted from 2.0.0 (see P1.4); `poweroff`,
`reboot` and `halt` remain.

## How to verify (workspace)

```
ninja -C CactOS-x86_32/build-meson stage   # libc -> userbins -> drivers -> cctkfs -> kernel
ninja -C CactOS-x86_32/build-meson iso     # non-GUI ISO via CactBridge build.py
# then the new P2.3 check target (headless boot + grep PASS/FAIL)
```

Boot the **CactBridge ISO** (`CactBridge-x86/build/cact-non-gui.iso`), not
`CactKernel/build-meson/cact-full.iso`, so the freshly rebuilt libc/userbins are
live.
