#!/usr/bin/env bash
# ==============================================================================
# cact_check.sh — headless boot check for CactOS (PLAN-2.0.0 P2.3).
#
# Boots a CactOS ISO in QEMU with no display, captures COM1 to a file, waits
# until the guest reaches userspace (or a deadline passes), then greps the boot
# log for the kernel self-test markers, the localrepo staging and for failure
# markers.  Exits non-zero if any required marker is missing or any failure
# marker is present, so the release can be gated with a single command:
#
#     ninja -C build-meson check
#
# ISO selection (first match wins):
#   1. $1 on the command line
#   2. $CACT_CHECK_ISO
#   3. ../CactBridge-x86/build/cact-check.iso     (preferred: cactcheck as init,
#                                                   runs the userspace self-tests)
#   4. ../CactBridge-x86/build/cact-non-gui.iso   (kernel self-tests only)
#   5. build-meson/cact-full.iso                  (kernel + LocalRepo cctkfs)
#
# Environment overrides:
#   CACT_CHECK_TIMEOUT   seconds before the boot is considered hung (default 120)
#   CACT_CHECK_SETTLE    extra seconds to keep running after userspace is up,
#                        so userspace self-tests have time to finish (default 10)
#   CACT_CHECK_MEM       guest RAM (default 2G)
#   CACT_CHECK_SMP       guest logical CPUs (default 2)
#   CACT_CHECK_LOG       serial log path (default: a temp file, kept on failure)
#   CACT_CHECK_KEEP_LOG  1 = keep the log even on success
#
# The kernel prints its self-test result on COM1; this script is the single
# source of truth for which markers mean "the kernel is healthy".
# ==============================================================================

set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

timeout_secs="${CACT_CHECK_TIMEOUT:-120}"
settle_secs="${CACT_CHECK_SETTLE:-10}"
mem="${CACT_CHECK_MEM:-2G}"
smp="${CACT_CHECK_SMP:-2}"
keep_log="${CACT_CHECK_KEEP_LOG:-0}"

# ------------------------------------------------------------------------------
# Resolve the ISO to boot.
# ------------------------------------------------------------------------------
iso="${1:-${CACT_CHECK_ISO:-}}"
if [[ -z "$iso" ]]; then
  for cand in \
      "$root/../CactBridge-x86/build/cact-check.iso" \
      "$root/../CactBridge-x86/build/cact-non-gui.iso" \
      "$root/build-meson/cact-full.iso" \
      "$root/build-meson/cact.iso"; do
    if [[ -f "$cand" ]]; then iso="$cand"; break; fi
  done
fi
if [[ -z "$iso" || ! -f "$iso" ]]; then
  echo "cact-check: no bootable ISO found — build one first:" >&2
  echo "  ninja -C build-meson iso-full   (needs LocalRepoCactOS-x86_32/cctkfs.img)" >&2
  echo "  or: ../CactBridge-x86/build.py --non-gui-iso" >&2
  exit 2
fi

log="${CACT_CHECK_LOG:-$(mktemp -t cact-check-XXXXXX.log)}"
if [[ -z "${CACT_CHECK_LOG:-}" ]]; then : >"$log"; fi

echo "cact-check: ISO   = $iso"
echo "cact-check: log   = $log"
echo "cact-check: QEMU  = -m $mem -smp $smp (timeout ${timeout_secs}s)"

# ------------------------------------------------------------------------------
# Build the QEMU command line.  KVM when the device is usable, TCG otherwise;
# headless (-display none) with COM1 redirected to the log file.
# ------------------------------------------------------------------------------
accel_args=(-accel tcg)
if [[ -w /dev/kvm ]]; then
  accel_args=(-accel kvm -cpu host)
fi

# The disk is optional (the kernel self-tests and the localrepo pipeline do not
# need it), and attaching it fails outright while a dev guest holds it: QEMU
# refuses to open an image another instance has locked, which used to surface as
# an empty log and a screenful of MISSes.  So attach it only when the lock is
# free — probe with the same fcntl write lock QEMU takes.
disk_args=()
disk_img="$root/build/nvme.img"
if [[ -f "$disk_img" ]]; then
  if python3 - "$disk_img" <<'PY'
import fcntl, sys
try:
    with open(sys.argv[1], "r+b") as f:
        fcntl.lockf(f, fcntl.LOCK_EX | fcntl.LOCK_NB)
except OSError:
    sys.exit(1)
PY
  then
    disk_args=(-drive file="$disk_img",if=none,id=sata0,format=raw
               -device ide-hd,drive=sata0,bus=ide.0)
  else
    echo "cact-check: $disk_img is in use (a dev guest is running) — booting without it" >&2
  fi
fi

qemu_args=(
  "${accel_args[@]}"
  -smp "$smp" -m "$mem" -M q35
  -cdrom "$iso" -boot d
  -display none -monitor none
  -serial "file:$log"
  -rtc base=localtime
  "${disk_args[@]}"
  -device qemu-xhci -device usb-kbd
  -no-reboot -no-shutdown
)

qemu-system-i386 "${qemu_args[@]}" >/dev/null 2>&1 &
qemu_pid=$!

cleanup() {
  kill "$qemu_pid" 2>/dev/null || true
  wait "$qemu_pid" 2>/dev/null || true
}
trap cleanup EXIT

boot_marker="Kernel is ready. Launching init..."
end_marker="CACT-CHECK: DONE"

start=$(date +%s)
seen_boot=0
settle_until=0

while :; do
  now=$(date +%s)
  elapsed=$((now - start))

  if grep -qF "$end_marker" "$log" 2>/dev/null; then
    break
  fi

  if [[ $seen_boot -eq 0 && -s "$log" ]] && grep -qF "$boot_marker" "$log" 2>/dev/null; then
    seen_boot=1
    settle_until=$((now + settle_secs))
  fi

  if [[ $seen_boot -eq 1 && $now -ge $settle_until ]]; then
    break
  fi

  if [[ $elapsed -ge $timeout_secs ]]; then
    echo "cact-check: TIMEOUT after ${timeout_secs}s" >&2
    break
  fi

  sleep 1
done

cleanup
trap - EXIT

# ------------------------------------------------------------------------------
# Analyse the captured log.
# ------------------------------------------------------------------------------
fail_re='PANIC|SELF-TEST FAILED|check\(s\) FAILED'
fail_re+='|CACT-CHECK: [A-Za-z0-9_]+ FAIL|CACT-CHECK: DONE.*fail=[1-9]'
# localrepo pipeline: a boot medium without the module, an empty /dev/localrepo
# or a failed/partial copy into tmpfs.
fail_re+='|devfs: no localrepo module|localrepo source is empty'
fail_re+='|localrepo copy failed|no localrepo archive source found'

# Markers that must be present for the check to pass.  These are the kernel's
# `tag: message` log lines; keep the list in sync with the self-tests in
# Cact/kernel/core/kernel.c.
required=(
  "csprng: selftest passed"
  "sigmagic: selftest passed"
  "energy: selftest passed"
  "timer: selftest OK"
  "placement: selftest passed"
  "mqueue: selftest passed"
)

rc=0

echo "cact-check: --- required markers ---"
for m in "${required[@]}"; do
  if grep -qF "$m" "$log"; then
    echo "cact-check:   ok    $m"
  else
    echo "cact-check:   MISS  $m"
    rc=1
  fi
done

# Re-check against the whole log rather than the polling flag: QEMU's file
# chardev can flush the boot marker and the end marker together, in which
# case the loop breaks on the end marker before noticing the boot marker.
if grep -qF "$boot_marker" "$log"; then
  echo "cact-check:   ok    reached userspace (init launched)"
else
  echo "cact-check:   MISS  reached userspace (init launched)"
  rc=1
fi

# Module ABI fingerprint: the kernel prints the ksym vermagic it will require of
# every module; the signer computes the same value from ksym.c.  If they differ,
# every module would be rejected at load, so gate on them matching.
vm_py="$(python3 "$root/tools/modsign.py" vermagic)"
echo "cact-check: --- module signing ---"
if grep -qF "sigmagic: ksym vermagic $vm_py" "$log"; then
  echo "cact-check:   ok    vermagic $vm_py (kernel == signer)"
else
  echo "cact-check:   MISS  vermagic $vm_py not in boot log (kernel/signer drift)"
  rc=1
fi

# Userspace tests: only checked when the image actually ran the runner (cactcheck
# staged as init).  A plain non-gui ISO has no CACT-CHECK lines and is judged on
# the kernel markers alone.
if grep -qF "CACT-CHECK: BEGIN" "$log"; then
  echo "cact-check: --- userspace tests (cactcheck) ---"
  grep -E "CACT-CHECK: ([a-z0-9_]+ (PASS|FAIL|SKIP)|DONE)" "$log" || true
  if grep -qF "CACT-CHECK: DONE" "$log"; then
    echo "cact-check:   ok    runner completed"
  else
    echo "cact-check:   MISS  runner did not complete (CACT-CHECK: DONE)"
    rc=1
  fi
  if grep -qE "CACT-CHECK: [a-z0-9_]+ PASS" "$log"; then
    echo "cact-check:   ok    at least one userspace test PASS"
  else
    echo "cact-check:   MISS  no userspace test PASS line"
    rc=1
  fi
fi

# The localrepo archive: the bootloader must have handed the kernel the
# `localrepo` module, and the supervisor must have copied it into tmpfs.  A
# medium built without the module (or with an empty /dev/localrepo) boots
# cleanly and only shows up much later as firmextr's confusing "not a readable
# zstd archive" at the shell, so both halves are hard failures here.
echo "cact-check: --- localrepo archive ---"
if grep -qF "devfs: localrepo module " "$log"; then
  grep -Fm1 "devfs: localrepo module " "$log" | sed 's/^/cact-check:   ok    /'
else
  echo "cact-check:   MISS  no localrepo module in the boot log" >&2
  echo "cact-check:         (this ISO carries no localrepo.tar.zst — /dev/localrepo reads empty)" >&2
  rc=1
fi
if grep -qF "cgoct: localrepo staged -> " "$log"; then
  grep -Fm1 "cgoct: localrepo staged -> " "$log" | sed 's/^/cact-check:   ok    /'
elif grep -qF "cgoct: supervisor online" "$log"; then
  echo "cact-check:   MISS  cgoct ran but never staged the archive into tmpfs" >&2
  rc=1
else
  echo "cact-check:   skip  no supervisor in this image (cgoct did not run)"
fi

# The boot cctkfs image: the kernel serves the module in place as /dev/cctkfs,
# which is what cact-install lays onto a target disk (the media carries no copy
# of the image).  The node is only there if the module was handed over, so its
# absence is a hard failure — an installer run would find no image source.
echo "cact-check: --- cctkfs image device ---"
if grep -qF "devfs: cctkfs module " "$log"; then
  grep -Fm1 "devfs: cctkfs module " "$log" | sed 's/^/cact-check:   ok    /'
else
  echo "cact-check:   MISS  no cctkfs module in the boot log — /dev/cctkfs reads empty" >&2
  rc=1
fi

echo "cact-check: --- failure markers ---"
if grep -nE "$fail_re" "$log"; then
  rc=1
else
  echo "cact-check:   none"
fi

if [[ $rc -eq 0 ]]; then
  echo "cact-check: RESULT: PASS"
  if [[ "$keep_log" != "1" && -z "${CACT_CHECK_LOG:-}" ]]; then rm -f "$log"; fi
else
  echo "cact-check: RESULT: FAIL (log kept at $log)" >&2
  tail -40 "$log" >&2 || true
fi

exit $rc
