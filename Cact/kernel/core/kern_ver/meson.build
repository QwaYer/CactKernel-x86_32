# Build timestamp, generated at build time into its own translation unit.
# Computing it at configure time (as before) stamped only the last `meson
# setup`, so the boot banner kept reporting an old time and made a freshly
# built kernel look stale.  `build_always_stale` re-runs `date` every build.
cact_build_time_src = custom_target('cact_build_time_src',
  output: 'cact_build_time.c',
  command: [python3, src_root / 'tools/gen_build_time.py', '@OUTPUT@'],
  build_always_stale: true,
)

kernel_libs += static_library('cact_kern_ver',
  files('version.c'),
  cact_build_time_src,
  include_directories: cact_includes,
  implicit_include_directories: false,
  c_args: kernel_c_args)
