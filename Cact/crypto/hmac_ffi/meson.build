# src/lib.rs embeds the module-signing key with include_bytes!, so the key has
# to exist before cargo runs (the Makefile generated it as a prerequisite of
# the archive).
if not fs.exists(meson.current_source_dir() / 'hmac_key.bin')
  run_command(python3, src_root / 'tools/gen_hmac_key.py', check: true)
endif

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

# `cact_hmac_ffi` is an rlib now and is linked into the kernel by
# `Cact/kernel/kernel_rust`, which is the kernel's single Rust archive.
