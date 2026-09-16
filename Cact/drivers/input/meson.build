kernel_objects += custom_target('asm-io',
  input: 'io.asm',
  output: 'io.o',
  command: [nasm, '-f', 'elf32', '@INPUT@', '-o', '@OUTPUT@'])

kernel_libs += static_library('cact_input',
  files(
    'keyboard.c',
    'mouse.c',
  ),
  include_directories: cact_includes,
  implicit_include_directories: false,
  c_args: kernel_c_args)
