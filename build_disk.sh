#!/usr/bin/env bash
# ==============================================================================
# Cact — пустой ext4-диск для QEMU
# ==============================================================================
# Создаёт только отформатированный ext4 без файлов. Userland — через cctkfs на ISO.
#
# Использование:
#   ./build_disk.sh
#
# Раньше скрипт собирал userspace и копировал ELF в образ — это убрано.
# Флаг --no-build оставлен для совместимости со старыми командами (ничего не делает).
# ==============================================================================

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

if [ "${1:-}" = "--no-build" ]; then
    : # совместимость: раньше пропускали сборку ELF
fi

IMG="build/nvme.img"
IMG_SIZE="${DISK_IMG_SIZE:-512M}"

cyan()  { printf '\033[1;36m%s\033[0m\n' "$*"; }
green() { printf '\033[1;32m%s\033[0m\n' "$*"; }
red()   { printf '\033[1;31m%s\033[0m\n' "$*" >&2; }

command -v qemu-img >/dev/null  || { red "qemu-img not found"; exit 1; }
command -v mkfs.ext4 >/dev/null || { red "mkfs.ext4 not found"; exit 1; }
command -v e2fsck >/dev/null   || { red "e2fsck not found"; exit 1; }

cyan "Creating empty ext4 image $IMG ($IMG_SIZE)..."
mkdir -p build
rm -f "$IMG"
qemu-img create -f raw "$IMG" "$IMG_SIZE" >/dev/null
mkfs.ext4 -q -L cactroot -b 4096 -I 128 -O ^64bit,^metadata_csum,^has_journal "$IMG"

e2fsck -fn "$IMG" >/dev/null 2>&1 || { red "e2fsck failed"; exit 1; }

green "Disk image ready (empty ext4, no files copied):"
echo "  $IMG"
echo
echo "Userland is supplied by the cctkfs module on the ISO; rebuild via CactOS-x86_32."
green "Run: ./run_qemu.sh"
