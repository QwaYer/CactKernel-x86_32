kernel_libs += static_library('cact_timer',
  files(
    'lapic_timer.c',
    'tick.c',
    'ktime.c',
  ),
  include_directories: cact_includes,
  implicit_include_directories: false,
  c_args: kernel_c_args)
