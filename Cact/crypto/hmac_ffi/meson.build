# The loadable-module signature is an ECDSA key pair now (see
# Cact/kernel/elf/mod_tag.c and tools/modsign.py), so this crate no longer
# embeds any signing secret; it only serves the /dev/crypto HMAC/HKDF requests.

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
