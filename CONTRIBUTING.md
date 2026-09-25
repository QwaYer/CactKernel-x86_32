# Contributing to CactKernel

CactKernel is the ring-0 half of CactOS. It is small on purpose: keep changes
tight, prove them in QEMU, and do not grow the kernel where a userspace service
or a VFS-node ioctl will do.

## Building

Requirements: `clang` with `-m32`, GNU `binutils`, `meson`, `ninja`, `cargo`
(nightly, for the Rust crates), and `grub-mkrescue` + `xorriso` for the ISO.

```sh
meson setup build-meson --cross-file cross/i686-cact-clang.ini
ninja -C build-meson            # kernel.bin + cact.iso
ninja -C build-meson iso-full   # cact-full.iso, with cctkfs.img
ninja -C build-meson clean
ninja -C build-meson rust-clean
```

Run the result with `./run_qemu.sh` — it picks `build-meson/cact-full.iso`,
then `build-meson/cact.iso`; override with `CACT_ISO`.

## Ground rules

1. **The trap ABI is fixed at 15 syscalls.** New functionality is a VFS-node
   ioctl, not a new syscall: add the command and its argument struct to
   `Cact/kernel/core/syscall/ioctl_abi.h`. `syscalls.h` and CactLibc's
   `include/syscall.h` must stay byte-for-byte identical; after a change,
   rebuild libc and re-link every user ELF.
2. **Keep the kernel minimal.** Services own their files and directories and
   configuration lives in `/etc`. Do not add kernel code just to make a
   userspace path writable — fix the program instead.
3. **Drivers register interrupts through `msidev_register()`** (MSI-X when the
   device offers it, otherwise a single MSI message). Out-of-tree modules must
   not touch the PIC.
4. **Ring-0 modules are built without SSE/MMX** (`-mno-sse -mno-sse2 -mno-mmx`)
   so the compiler cannot clobber a user process's FPU state.
5. **Modules are signed.** A `.cctk` carries an HMAC-SHA256 tag appended by
   `tools/cact_sign.py` (key generated with `tools/gen_hmac_key.py`); the loader
   verifies it before running the code.

## Testing

There is no unit-test suite in this tree — verification is a boot in QEMU.
Reboot after any userspace change: `/bin` and `/sbin` come from the cctkfs
image, not the working tree. Kernel output goes to COM1, which `run_qemu.sh`
wires to `-serial stdio`.

## Style

Match the surrounding file. Cross-file C symbols carry a `cact_`/`sys_` prefix;
anything the C side calls from Rust is `#[no_mangle] extern "C"`. Comments
explain *why* a line exists — the constraint or hardware quirk — not what it
does. Commit messages may be in Russian or English; state what changed and why,
and note how you verified it.

## License

By contributing you agree that your work is licensed under the repository's
**GPLv3** (see [`LICENSE`](LICENSE)).
