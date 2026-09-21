_rust_inputs = []
# The archive also links this crate's in-repo path dependencies (cact_crypto and
# cact_shim), so their sources must be inputs too: without them ninja considers
# libcact_net.a up to date after an edit to the crypto crate.  Vendored crates
# under Cact/crypto/vendor are pruned (imported source drops, not edited here),
# as is hmac_ffi (a separate crate with its own target).
_crypto_src = meson.project_source_root() / 'Cact/crypto'
_found = run_command('sh', '-c',
  'find ' + meson.current_source_dir() + ' ' + _crypto_src +
  ' \\( -name target -o -name vendor -o -name hmac_ffi \\) -prune -o -type f -print | LC_ALL=C sort',
  check: true)

foreach f : _found.stdout().strip().split('\n')
  if f != ''
    _rust_inputs += f
  endif
endforeach

kernel_rust_libs += custom_target('rust-cact-net',
  input: _rust_inputs,
  output: 'libcact_net.a',
  command: [bash, rust_build_script, meson.current_source_dir(),
            'libcact_net.a', 'nightly', '@OUTPUT@',
            '-Z', 'json-target-spec',
            '-Z', 'build-std=core,alloc,compiler_builtins',
            '-Z', 'build-std-features=compiler-builtins-mem'],
  build_by_default: false)
