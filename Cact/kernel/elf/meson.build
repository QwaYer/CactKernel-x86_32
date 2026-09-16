kernel_libs += static_library('cact_elf',
  files(
    'elf_loader.c',
    'ksym.c',
    'sym.c',
  ),
  include_directories: cact_includes,
  implicit_include_directories: false,
  c_args: kernel_c_args)
