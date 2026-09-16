# The SMP trampoline is assembled as a flat binary and embedded in the link.
#
# The Makefile embedded it with
#     ld -m elf_i386 -r -b binary build/trampoline.bin
# which derives symbol names from the path given on the command line, and
# sched/src/smp.rs declares them as _binary_build_trampoline_bin_{start,end}.
# objcopy names symbols after the path it is handed, which here is the source
# path (Cact/kernel/smp/trampoline.bin) relative to the build directory;
# --redefine-sym maps those onto the names the kernel actually links against,
# so the Rust interface no longer depends on the build directory's name.
trampoline_bin = custom_target('trampoline-bin',
  input: 'trampoline.asm',
  output: 'trampoline.bin',
  command: [nasm, '-f', 'bin', '@INPUT@', '-o', '@OUTPUT@'])

trampoline_blob = custom_target('trampoline-blob',
  input: trampoline_bin,
  output: 'trampoline-blob.o',
  command: [objcopy, '-I', 'binary', '-O', 'elf32-i386', '-B', 'i386',
            '@INPUT@', '@OUTPUT@'])

kernel_objects += custom_target('trampoline-obj',
  input: trampoline_blob,
  output: 'trampoline.o',
  command: [objcopy,
            '--redefine-sym', '_binary_Cact_kernel_smp_trampoline_bin_start=_binary_build_trampoline_bin_start',
            '--redefine-sym', '_binary_Cact_kernel_smp_trampoline_bin_end=_binary_build_trampoline_bin_end',
            '--redefine-sym', '_binary_Cact_kernel_smp_trampoline_bin_size=_binary_build_trampoline_bin_size',
            '@INPUT@', '@OUTPUT@'])
