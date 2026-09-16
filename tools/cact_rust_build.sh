#!/usr/bin/env bash
#
# Build one of the kernel's Rust static libraries and stage the archive at a
# path Meson can depend on.
#
# Usage:
#   cact_rust_build.sh <crate-dir> <lib-name> <nightly|stable> <output> [cargo args...]
#
# The crate's .cargo/config.toml selects the i686-cact target; the -Z flags the
# Makefile used to pass per crate arrive through the trailing arguments (they are
# not all expressible in .cargo/config.toml, e.g. rust_net needs alloc in
# build-std).  CARGO_TARGET_DIR is honoured the same way the Makefile honoured
# it.
set -euo pipefail

crate_dir=$1
lib_name=$2
toolchain=$3
# Meson hands over a path relative to the build directory; resolve it before the
# cd below makes it meaningless.
output=$(readlink -m "$4")
shift 4

cd "$crate_dir"

case "$toolchain" in
  nightly) cargo +nightly build --release "$@" ;;
  stable)  cargo build --release "$@" ;;
  *)
    echo "cact_rust_build.sh: unknown toolchain selector '$toolchain'" >&2
    exit 2
    ;;
esac

target_dir=${CARGO_TARGET_DIR:-target}
cp "$target_dir/i686-cact/release/$lib_name" "$output"
