#!/usr/bin/env bash
#
# Assemble a GRUB-bootable ISO for the Cact kernel.
#
# Usage: cact_make_iso.sh <kernel.bin> <grub.cfg> <isodir> <out.iso> [cctkfs.img]
#                                                     [localrepo.tar.zst]
#
# With a cctkfs image the module is copied in and signed, which is what the
# Makefile's iso-full target did.  The staging directory is recreated every
# time so a stale cctkfs.img can never leak into a kernel-only image.
#
# The localrepo archive rides along as a second Multiboot2 module: the kernel
# serves it as /dev/localrepo for cgoct to copy into tmpfs.  The grub.cfg
# placeholder line is filled in when the archive is passed and dropped
# otherwise, so a kernel-only image still boots (same contract as CactBridge).
set -euo pipefail

kernel=$1
grub_cfg=$2
isodir=$3
output=$4
cctkfs=${5:-}
localrepo=${6:-}

rm -rf "$isodir"
mkdir -p "$isodir/boot/grub"
cp "$kernel" "$isodir/boot/kernel.bin"

if [ -n "$cctkfs" ]; then
  if [ ! -f "$cctkfs" ]; then
    echo "cact_make_iso.sh: cctkfs image not found: $cctkfs" >&2
    exit 1
  fi
  cp "$cctkfs" "$isodir/boot/cctkfs.img"
  python3 "$(dirname "$(readlink -f "$0")")/cact_sign_cctkfs.py" "$isodir/boot/cctkfs.img"
fi

if [ -n "$localrepo" ] && [ ! -f "$localrepo" ]; then
  echo "cact_make_iso.sh: localrepo archive not found: $localrepo" >&2
  exit 1
fi

cp "$grub_cfg" "$isodir/boot/grub/grub.cfg"

if [ -n "$localrepo" ]; then
  cp "$localrepo" "$isodir/boot/localrepo.tar.zst"
  sed -i 's|LOCALREPO_ISO_PLACEHOLDER|localrepo.tar.zst|' \
      "$isodir/boot/grub/grub.cfg"
else
  sed -i '/LOCALREPO_ISO_PLACEHOLDER/d' "$isodir/boot/grub/grub.cfg"
  echo "cact_make_iso.sh: no localrepo archive — booting without /dev/localrepo" >&2
fi

grub-mkrescue -o "$output" "$isodir"
