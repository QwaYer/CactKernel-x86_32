# ---------------------------------------------------------------------------
# Third-party ACPICA interpreter.
#
# acpica/ is a git checkout that is not part of this repository, so it is
# cloned on demand and patched for freestanding use at configure time, the same
# way the Makefile did it while parsing.  Only the nine component directories
# the kernel uses are compiled.
# ---------------------------------------------------------------------------

acpica_dir = meson.current_source_dir() / 'acpica'

if not fs.is_dir(acpica_dir / 'source/include')
  if not get_option('acpica_fetch')
    error('ACPICA sources are missing at ' + acpica_dir + ' and -Dacpica_fetch=false')
  endif
  if not git.found()
    error('git is required to fetch ACPICA, or configure with -Dacpica_fetch=false')
  endif
  message('Cloning ACPICA from ' + get_option('acpica_repo') + ' ...')
  run_command(git, 'clone', '--depth', '1', get_option('acpica_repo'), acpica_dir,
    check: true)
endif

run_command(python3, src_root / 'tools/cact_acpica_patch.py', acpica_dir, check: true)

acpica_components = [
  'tables',
  'namespace',
  'parser',
  'dispatcher',
  'executer',
  'hardware',
  'events',
  'resources',
  'utilities',
]

_acpica_dirs = []
foreach c : acpica_components
  _acpica_dirs += 'acpica' / 'source' / 'components' / c
endforeach

# utclib/utprint/rsdump* duplicate libc/klib symbols.  hwxfsleep.c is the
# ACPI sleep/wake entry point set (AcpiEnterSleepState[Prep], AcpiLeaveSleepState*,
# AcpiSetFirmwareWakingVector, AcpiEnterSleepStateS4bios); it is compiled in now
# that Cact owns an ACPI power path (drivers/acpi/power.c).
acpica_exclude = ['utclib.c', 'utprint.c', 'rsdump.c', 'rsdumpinfo.c']

_acpica_found = run_command('sh', '-c',
  'cd ' + meson.current_source_dir() + ' && find ' + ' '.join(_acpica_dirs) +
  ' -type f -name "*.c" | LC_ALL=C sort', check: true)

acpica_sources = []
foreach s : _acpica_found.stdout().strip().split('\n')
  if s == ''
    continue
  endif
  keep = true
  foreach e : acpica_exclude
    if s.endswith('/' + e)
      keep = false
    endif
  endforeach
  if keep
    acpica_sources += s
  endif
endforeach

acpica_c_args = kernel_c_args + [
  '-Wno-unused-parameter',
  '-Wno-unused-variable',
  '-Wno-sign-compare',
  '-Wno-missing-braces',
  '-Wno-strict-aliasing',
  '-Wno-implicit-function-declaration',
  '-DACPI_MUTEX_TYPE=0',
]

kernel_libs += static_library('acpica',
  acpica_sources,
  include_directories: cact_includes,
  implicit_include_directories: false,
  c_args: acpica_c_args)

kernel_libs += static_library('cact_acpi',
  files(
    'acpi.c',
    'power.c',
    'osl.c',
    'osl_sync.c',
    'osl_io.c',
    'apic.c',
  ),
  include_directories: cact_includes,
  implicit_include_directories: false,
  c_args: kernel_c_args)

# S3-resume trampoline: a flat real-mode binary staged in conventional memory
# and advertised as the FACS waking vector.  Embedded exactly like the SMP AP
# trampoline, with the path-derived objcopy symbols renamed to stable names.
wake_bin = custom_target('acpi-wake-bin',
  input: 'wake_trampoline.asm',
  output: 'wake_trampoline.bin',
  command: [nasm, '-f', 'bin', '@INPUT@', '-o', '@OUTPUT@'])

wake_blob = custom_target('acpi-wake-blob',
  input: wake_bin,
  output: 'wake_trampoline_blob.o',
  command: [objcopy, '-I', 'binary', '-O', 'elf32-i386', '-B', 'i386',
            '@INPUT@', '@OUTPUT@'])

kernel_objects += custom_target('acpi-wake-obj',
  input: wake_blob,
  output: 'wake_trampoline.o',
  command: [objcopy,
            '--redefine-sym', '_binary_Cact_drivers_acpi_wake_trampoline_bin_start=_binary_cact_wake_bin_start',
            '--redefine-sym', '_binary_Cact_drivers_acpi_wake_trampoline_bin_end=_binary_cact_wake_bin_end',
            '--redefine-sym', '_binary_Cact_drivers_acpi_wake_trampoline_bin_size=_binary_cact_wake_bin_size',
            '@INPUT@', '@OUTPUT@'])

# The context save/enter-sleep/resume stub, linked as normal kernel code.
kernel_objects += custom_target('asm-acpi-wake-entry',
  input: 'wake_entry.asm',
  output: 'wake_entry.o',
  command: [nasm, '-f', 'elf32', '@INPUT@', '-o', '@OUTPUT@'])
