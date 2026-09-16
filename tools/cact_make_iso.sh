#!/usr/bin/env bash
#
# Assemble a GRUB-bootable ISO for the Cact kernel.
#
# Usage: cact_make_iso.sh <kernel.bin> <grub.cfg> <isodir> <out.iso> [cctkfs.img]
#
# With a cctkfs image the module is copied in and signed, which is what the
# Makefile's iso-full target did.  The staging directory is recreated every
# time so a stale cctkfs.img can never leak into a kernel-only image.
set -euo pipefail

kernel=$1
grub_cfg=$2
isodir=$3
output=$4
cctkfs=${5:-}

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

cp "$grub_cfg" "$isodir/boot/grub/grub.cfg"
grub-mkrescue -o "$output" "$isodir"
