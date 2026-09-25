#!/usr/bin/env bash
# Launch QEMU for CactKernel.
# On the first run without a disk: ./build_disk.sh is called automatically (empty ext4).
# Full cycle (drivers + cctkfs + ISO + disk): ../build-cact-qemu.sh
# Fixed: the monitor is moved to a virtual console (Ctrl+Alt+2 in the QEMU window),
# so that it does not conflict with the serial port stdio.
#
# GDB debugging:  QEMU_GDB=1 ./run_qemu.sh  or  ./run_qemu_gdb.sh
#   QEMU listens on tcp::1234, the guest halts until "continue" in gdb.
#   Build with symbols:  meson configure build-meson -Dkern_debug=true && ninja -C build-meson
#   Session:  gdb -x gdb/cact.gdb

set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

if [[ ! -f build/nvme.img ]]; then
    echo "[run_qemu] build/nvme.img is missing — creating it via ./build_disk.sh" >&2
    ./build_disk.sh
fi

if [[ ! -v QEMU_DEBUG ]]; then
    QEMU_DEBUG="int,cpu_reset,guest_errors,mmu"
fi

LOG_ARGS=()
if [[ -n "$QEMU_DEBUG" ]]; then
    LOG_ARGS=(-d "$QEMU_DEBUG" -D "${QEMU_LOG:-qemu.log}")
fi

QEMU_EXTRA=()
if [[ -n "${QEMU_GDB:-}" ]]; then
    QEMU_EXTRA+=(-gdb tcp::1234 -S)
fi

ISO="${CACT_ISO:-}"
if [[ -z "$ISO" ]]; then
  # Meson build dir. The full ISO is preferred: the kernel-only image panics
  # without the cctkfs module (see README).
  for cand in build-meson/cact-full.iso build-meson/cact.iso; do
    if [[ -f "$SCRIPT_DIR/$cand" ]]; then
      ISO="$SCRIPT_DIR/$cand"
      break
    fi
  done
fi
if [[ -z "$ISO" || ! -f "$ISO" ]]; then
  echo "[run_qemu] Build the ISO: ninja -C build-meson (iso-full for a bootable one) — or set CACT_ISO" >&2
  exit 1
fi

exec qemu-system-i386 \
    -accel kvm \
    -cpu host \
    -smp 4 \
    -m 4G \
    -cdrom "$ISO" \
    -boot d \
    -serial stdio \
    -rtc base=localtime \
    -M q35 \
    -display gtk,gl=on \
    -monitor vc \
    -drive file=build/nvme.img,if=none,id=sata0,format=raw \
    "${LOG_ARGS[@]}" \
    -device ide-hd,drive=sata0,bus=ide.0 \
    -netdev user,id=u1 \
    -device virtio-net-pci,disable-modern=on,netdev=u1 \
    -device virtio-vga-gl \
    -object filter-dump,id=dump0,netdev=u1,file=/tmp/net.pcap \
    -device qemu-xhci,id=xhci -device usb-kbd \
    -no-reboot \
    -no-shutdown \
    "${QEMU_EXTRA[@]}"
