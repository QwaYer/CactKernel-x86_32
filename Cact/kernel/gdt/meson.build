kernel_objects += custom_target('asm-gdt',
  input: 'gdt.asm',
  output: 'gdt_asm.o',
  command: [nasm, '-f', 'elf32', '@INPUT@', '-o', '@OUTPUT@'])

kernel_libs += static_library('cact_gdt',
  files('gdt.c'),
  include_directories: cact_includes,
  implicit_include_directories: false,
  c_args: kernel_c_args)
