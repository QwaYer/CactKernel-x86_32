#!/usr/bin/env bash
# Запуск QEMU для CactKernel.
# При первом запуске без диска: автоматически вызывается ./build_disk.sh (пустой ext4).
# Полный цикл (драйверы + cctkfs + ISO + диск): ../build-cact-qemu.sh
# Исправлено: монитор вынесен в виртуальную консоль (Ctrl+Alt+2 в окне QEMU), 
# чтобы не конфликтовать со stdio серийного порта.
#
# Отладка GDB:  QEMU_GDB=1 ./run_qemu.sh  или  ./run_qemu_gdb.sh
#   QEMU слушает tcp::1234, гость стоит до "continue" в gdb.
#   Сборка с символами:  meson configure build-meson -Dkern_debug=true && ninja -C build-meson
#   Сессия:  gdb -x gdb/cact.gdb

set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

if [[ ! -f build/nvme.img ]]; then
    echo "[run_qemu] build/nvme.img отсутствует — создаю через ./build_disk.sh" >&2
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
  echo "[run_qemu] Соберите ISO: ninja -C build-meson (iso-full для загрузочного) — или задайте CACT_ISO" >&2
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
    -vga std \
    -display gtk \
    -monitor vc \
    -drive file=build/nvme.img,if=none,id=sata0,format=raw \
    "${LOG_ARGS[@]}" \
    -device ide-hd,drive=sata0,bus=ide.0 \
    -netdev user,id=u1 \
    -device virtio-net-pci,disable-modern=on,netdev=u1 \
    -object filter-dump,id=dump0,netdev=u1,file=/tmp/net.pcap \
    -device qemu-xhci,id=xhci -device usb-kbd \
    -no-reboot \
    -no-shutdown \
    "${QEMU_EXTRA[@]}"
