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

kernel_rust_libs += custom_target('rust-cact-net',
  input: _rust_inputs,
  output: 'libcact_net.a',
  command: [bash, rust_build_script, meson.current_source_dir(),
            'libcact_net.a', 'nightly', '@OUTPUT@',
            '-Z', 'json-target-spec',
            '-Z', 'build-std=core,alloc,compiler_builtins',
            '-Z', 'build-std-features=compiler-builtins-mem'],
  build_by_default: false)
