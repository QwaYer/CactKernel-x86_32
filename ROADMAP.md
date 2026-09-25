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
  the VT/PTY layer has a self-test (`devtest` in CactUserBins).

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
