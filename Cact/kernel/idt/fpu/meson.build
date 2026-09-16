kernel_objects += custom_target('asm-fpu-entry',
  input: 'fpu_entry.asm',
  output: 'fpu_entry.o',
  command: [nasm, '-f', 'elf32', '@INPUT@', '-o', '@OUTPUT@'])

kernel_libs += static_library('cact_fpu',
  files('fpu_hd.c'),
  include_directories: cact_includes,
  implicit_include_directories: false,
  c_args: kernel_c_args)
