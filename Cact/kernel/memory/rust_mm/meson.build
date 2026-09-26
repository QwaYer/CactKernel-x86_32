# rust_mm uses the stable cargo front-end; its .cargo/config.toml points at
# i686-cact.json and enables core/build-std.
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

# `cact_mm` is an rlib now; the kernel's single Rust archive is built by
# `Cact/kernel/kernel_rust` (see its src/lib.rs for why).
