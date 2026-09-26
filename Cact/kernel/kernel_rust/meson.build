# The kernel's single Rust archive.  Every kernel Rust crate is an rlib, so
# they call each other with plain `use`; this staticlib depends on all of them
# and pulls them into one archive, which is what keeps every symbol (and every
# `#[no_mangle]` C-ABI entry point) present exactly once in kernel.bin.
#
# The scanned directories are the whole Rust graph: a change anywhere in it must
# rebuild this archive.
_rust_scan_dirs = [
  meson.current_source_dir(),
  meson.project_source_root() / 'Cact/kernel/memory/rust_mm',
  meson.project_source_root() / 'Cact/kernel/proc/sched',
  meson.project_source_root() / 'Cact/kernel/sync',
  meson.project_source_root() / 'Cact/drivers/gpu/drm/rust_drm',
  meson.project_source_root() / 'Cact/drivers/gpu/kms',
  meson.project_source_root() / 'Cact/net/rust_net',
  meson.project_source_root() / 'Cact/crypto',
]

_rust_inputs = []
_found = run_command('sh', '-c',
  'for d in ' + ' '.join(_rust_scan_dirs) + '; do ' +
  'find "$d" \\( -name target -o -name vendor \\) -prune -o -type f -print; done | LC_ALL=C sort',
  check: true)

foreach f : _found.stdout().strip().split('\n')
  if f != ''
    _rust_inputs += f
  endif
endforeach

kernel_rust_libs += custom_target('rust-cact-kernel',
  input: _rust_inputs,
  output: 'libcact_kernel.a',
  command: [bash, rust_build_script, meson.current_source_dir(),
            'libcact_kernel.a', 'nightly', '@OUTPUT@',
            '-Z', 'json-target-spec',
            '-Z', 'build-std=core,alloc,compiler_builtins',
            '-Z', 'build-std-features=compiler-builtins-mem'],
  build_by_default: false)
