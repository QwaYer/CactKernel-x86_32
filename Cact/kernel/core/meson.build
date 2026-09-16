kernel_objects += custom_target('asm-kernel-entry',
  input: 'kernel.asm',
  output: 'kernel_entry.o',
  command: [nasm, '-f', 'elf32', '@INPUT@', '-o', '@OUTPUT@'])

kernel_objects += custom_target('asm-exception-stubs',
  input: 'exception_stubs.asm',
  output: 'exception_stubs.o',
  command: [nasm, '-f', 'elf32', '@INPUT@', '-o', '@OUTPUT@'])

kernel_objects += custom_target('asm-device-isrs',
  input: 'device_isrs.asm',
  output: 'device_isrs.o',
  command: [nasm, '-f', 'elf32', '@INPUT@', '-o', '@OUTPUT@'])

kernel_objects += custom_target('asm-msix-stubs',
  input: 'msix_stubs.asm',
  output: 'msix_stubs.o',
  command: [nasm, '-f', 'elf32', '@INPUT@', '-o', '@OUTPUT@'])

kernel_objects += custom_target('asm-syscall-entries',
  input: 'syscall_entries.asm',
  output: 'syscall_entries.o',
  command: [nasm, '-f', 'elf32', '@INPUT@', '-o', '@OUTPUT@'])

kernel_libs += static_library('cact_core',
  files(
    'kernel.c',
    'kernel_exc.c',
    'kernel_boot.c',
    'multiboot2.c',
    'klib.c',
    'klog.c',
    'serial.c',
    'stack_guard.c',
  ),
  include_directories: cact_includes,
  implicit_include_directories: false,
  c_args: kernel_c_args)
