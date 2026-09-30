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

## TLS

Userspace HTTPS now runs through the **libc** TLS 1.3 client (`CactLibc`), not
the kernel. Today it supports X25519, `TLS_AES_128_GCM_SHA256` and RSA/ECDSA
server certificates. Still missing: **client certificates, PSK, session
resumption and TLS 1.2**. The in-kernel rustls path remains only for the kernel
HTTP client.

## Interrupts & drivers

- MSI-X is the preferred device interrupt, with a single **MSI** message as the
  fallback (`msidev_register()`); both paths need coverage across the driver set.
- **GPU / KMS**: the DRM uapi and a virtio-gpu driver exist as a sibling repo;
  the VT/PTY layer has a self-test (`devtest` in CactUserBins). No Intel code
  exists anywhere in the tree — see [GPU / Intel (i915)](#gpu--intel-i915) for
  what a port still needs from the kernel.

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

## Threads & POSIX pthreads

Assessed 2026-09-30. **No threads exist.** `pthread_create()` in
`CactLibc/src/pthread.c` is a stub returning `ENOSYS`; there is no `clone`, no
`futex`, and no `tid`. A `task_struct` owns its own `proc_metadata_t` and page
directory, and `task_reap` tears all of it down at process exit — so "several
threads, one address space" is a new model, not a flag.

**ABI decision:** the 15-trap syscall set stays final. Thread creation and
futex join as ioctl ranges — thread ops on `/proc/self/ctl` (`CACT_PROCCTL_*`),
wait/wake on a new `/dev/futex` node — exactly how the rest of the kernel
exposes non-trap facilities.

### Kernel prerequisites

- **Thread identity.** Add `tid` (unique) and `tgid` (thread-group leader) to
  `task_struct`; `pid` stays the process id and a `find_task_by_tid` joins the
  pid lookup. `/proc` lists only group leaders under `<pid>/`.
- **Shared address space.** `proc_metadata_t` gains a shared-mm refcount and a
  live-thread count; threads share `proc` and `page_directory`. `task_reap` and
  `SYS_EXIT` free `mm`/fds/cwd/`proc_metadata_t` **only when the last thread
  exits**; a non-last thread frees just its kernel stack and `task_struct`.
  COW / `vmm_free_address_space` must not run twice.
- **Thread create.** `CACT_PROCCTL_THREAD_CREATE {entry, stack_top, arg,
  tls_base, detached}` allocates a task + kernel stack, shares the mm, and arms
  a trap frame so the first schedule `iretd`s to `entry` in ring 3 with
  `esp = stack_top` and `eax = arg`. Returns the new `tid`.
- **TLS.** Per-thread `%gs` base (a GDT entry per thread, or one selector with a
  per-task base field loaded by `switch_to`) — needed for `errno` and
  `pthread_*` keys.
- **Futex.** `CACT_FUTEX_WAIT {uaddr, expected, timeout}` /
  `CACT_FUTEX_WAKE {uaddr, n}`, keyed by (shared mm, uaddr), parked on the
  scheduler `sleep_queue`; `FUTEX_WAIT` re-checks the word under the mm lock to
  close the lost-wakeup race.
- **Signals.** Handlers and pending signals are process-wide (already in
  `proc_metadata_t`); per-thread masks (`pthread_sigmask`) need a mask on
  `task_struct`. `SIGSEGV`/`SIGFPE`/`SIGILL` terminate the whole group.
- **Entry alignment.** A thread's first ring-3 entry must satisfy the i386 SSE
  contract (`esp ≡ 12 mod 16`), the same invariant the fixed `start.S` now
  establishes; the trampoline lands via `iretd`, so set `stack_top` accordingly.

### libc (`CactLibc`) — full POSIX set

- **Threads:** `pthread_create`/`_join`/`_exit`/`_detach`/`_self`/`_equal`,
  `pthread_attr_*` (stack size, detach state, guard size), `pthread_kill`,
  `pthread_sigmask`.
- **Synchronisation:** mutex (normal, error-check, recursive; `_timedlock`),
  condition variables (`_wait`/`_timedwait`/`_signal`/`_broadcast`), read/write
  locks (`rdlock`/`wrlock`/`trylock`/`timed*`/`unlock`), spinlocks, barriers,
  `pthread_once`.
- **TLS & lifecycle:** `pthread_key_create`/`_delete`/`setspecific`/`getspecific`
  backed by real `%gs` TLS (today's globals are per-process only),
  `pthread_cleanup_push`/`_pop` (needs an unwind-aware trampoline), and deferred
  **cancellation** (`pthread_cancel`/`_testcancel`/`_setcancelstate`/`_setcanceltype`).
- **Semaphores:** POSIX `sem_*` on the same futex.
- Everything futex-backed; the stubs in `src/pthread.c` are replaced, not wrapped.

### Phases

1. Kernel identity + shared mm/refcount + per-thread `SYS_EXIT` semantics.
2. `THREAD_CREATE` + TLS base + entry alignment; a ring-3 smoke test where two
   threads share a global and exit.
3. `/dev/futex` wait/wake; mutex/cond in libc.
4. Full libc surface (rwlock, barriers, once, cleanup, cancel, per-thread signal
   masks) and a multithreaded self-test in `CactUserBins`.

**Acceptance:** `pthread_create` runs two threads on different CPUs, a mutex
serialises a counter to the expected total, `join` returns the thread's value,
and a thread `SIGSEGV`/`exit` tears down the group without leaking the address
space.

## Storage & filesystems

- **ext4** is read/write in-tree; FAT32 ships as an out-of-tree `.cctk` module.
- The page cache and optional **swap** are in place; a swap partition that
  cannot be used today only logs a warning.

## ABI & userspace

- The **15-trap** syscall ABI is considered final. New subsystems join as ioctl
  ranges in `ioctl_abi.h` (fd, dir, proc, socket, net, sys, pipe, crypto, tty,
  pty), never as new traps.
- All repos build with **Meson + Ninja**; the toolchain is `clang -m32` against
  the workspace's own libc.
