# The sync crate is a path dependency of sched (and is listed as a prerequisite
# of the sched archive in the Makefile), so its sources are scanned as build
# inputs here rather than built into an archive of their own.  The
# "find ... -name target -prune" keeps cargo's output directory out of the
# dependency list while still tracking sources, manifests and target specs.
sched_scan_dirs = [
  meson.current_source_dir(),
  meson.current_source_dir() / '../sync',
]

_sched_inputs = []
_sched_found = run_command('sh', '-c',
  'for d in ' + ' '.join(sched_scan_dirs) + '; do ' +
  'find "$d" -name target -prune -o -type f -print; done | LC_ALL=C sort',
  check: true)

foreach f : _sched_found.stdout().strip().split('\n')
  if f != ''
    _sched_inputs += f
  endif
endforeach

# `sched` is an rlib now; the kernel's single Rust archive is built by
# `Cact/kernel/kernel_rust` (see its src/lib.rs for why).
