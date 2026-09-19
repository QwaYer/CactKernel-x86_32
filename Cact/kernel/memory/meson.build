kernel_objects += custom_target('asm-mm',
  input: 'mm.asm',
  output: 'mm.o',
  command: [nasm, '-f', 'elf32', '@INPUT@', '-o', '@OUTPUT@'])

kernel_libs += static_library('cact_memory',
  files('pat.c', 'mtrr.c'),
  include_directories: cact_includes,
  implicit_include_directories: false,
  c_args: kernel_c_args)
