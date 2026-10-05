#!/usr/bin/env bash
# ==============================================================================
# cact_check.sh — headless boot check for CactOS (PLAN-2.0.0 P2.3).
#
# Boots a CactOS ISO in QEMU with no display, captures COM1 to a file, waits
# until the guest reaches userspace (or a deadline passes), then greps the boot
# log for the kernel self-test markers and for failure markers.  Exits non-zero
# if any required marker is missing or any failure marker is present, so the
# release can be gated with a single command:
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

disk_args=()
if [[ -f "$root/build/nvme.img" ]]; then
  disk_args=(-drive file="$root/build/nvme.img",if=none,id=sata0,format=raw
             -device ide-hd,drive=sata0,bus=ide.0)
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

# Markers that must be present for the check to pass.  Keep this in sync with
# the kernel self-tests in Cact/kernel/core/kernel.c.
required=(
  "csprng      : selftest passed"
  "sigmagic    : selftest passed"
  "energy      : selftest passed"
  "timer       : selftest OK"
  "placement   : selftest passed"
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
if grep -qF "sigmagic    : ksym vermagic $vm_py" "$log"; then
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
