# The DRM/KMS core's dominant half: the Rust crate that owns the object model
# and the ioctl decoding.  Built like rust_mm / rust_net — a cargo staticlib
# with its own i686-cact target, linked into the kernel inside the --start-group
# with the other Rust archives.  The C sources next to it (core/, gem/, kms/)
# are the ABI and VFS glue around it and shrink as slices move here.
_rust_inputs = []
_found = run_command('sh', '-c',
  'find ' + meson.current_source_dir() +
  ' -name target -prune -o -type f -print | LC_ALL=C sort',
  check: true)

foreach f : _found.stdout().strip().split('\n')
  if f != ''
    _rust_inputs += f
  endif
endforeach

# The KMS sources live next door (`Cact/drivers/gpu/kms/src`) and are part of
# this crate via `#[path]`; list them as inputs too, or editing them would not
# trigger a rebuild.
_found_kms = run_command('sh', '-c',
  'find ' + meson.current_source_dir() + '/../../kms/src' +
  ' -type f -print | LC_ALL=C sort',
  check: true)

foreach f : _found_kms.stdout().strip().split('\n')
  if f != ''
    _rust_inputs += f
  endif
endforeach

# `cact_drm` is an rlib now; the kernel's single Rust archive is built by
# `Cact/kernel/kernel_rust` (see its src/lib.rs for why).
