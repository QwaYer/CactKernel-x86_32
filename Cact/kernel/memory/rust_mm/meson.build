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

kernel_rust_libs += custom_target('rust-cact-mm',
  input: _rust_inputs,
  output: 'libcact_mm.a',
  command: [bash, rust_build_script, meson.current_source_dir(),
            'libcact_mm.a', 'stable', '@OUTPUT@'],
  build_by_default: false)
