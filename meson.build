# ==============================================================================
# Cact Kernel — Meson build system (aggregating root)
# ==============================================================================
#
#   meson setup build-meson --cross-file cross/i686-cact-clang.ini
#   ninja -C build-meson                 # kernel.bin + cact.iso  (make all)
#   ninja -C build-meson iso-full        # cact-full.iso         (make iso-full)
#
# The Makefile is kept alongside this file; both build the same artifacts.
#
# Every source directory has its own meson.build and produces its own static
# library or object file.  This file only sets up the toolchain, the shared
# flags and the final link, then visits the directories in dependency-free
# order.  Sources are compiled by clang (32-bit, freestanding) and linked by
# lld/ld against linker.ld.
#
# The per-directory C archives are linked with --whole-archive because the
# Makefile fed every object file to the linker individually: the kernel has no
# single entry point that pulls everything in (linker-script symbols, init
# tables and the Rust <-> C boundary all reference objects "from the side"), so
# plain archive semantics could silently drop code.  The Rust archives keep
# normal archive semantics inside a --start-group, exactly as before.
# ==============================================================================

project('cact-kernel', 'c',
  meson_version: '>=1.0.0',
  default_options: [
    # ACPICA calls isxdigit()/isdigit() without declaring them, which clang
    # rejects outright in C23 (GCC still only warns).  gnu17 is clang's own
    # default too; spelling it out keeps the ABI from depending on the
    # compiler's default.
    'c_std=gnu17',
    'warning_level=1',   # -Wall, as in the Makefile
    'buildtype=plain',   # no -O/-g beyond what kern_debug asks for
    'b_staticpic=false',
    'b_pie=false',
    'b_ndebug=false',
  ])

fs = import('fs')
cc = meson.get_compiler('c')

src_root = meson.project_source_root()
build_root = meson.current_build_dir()

bash = find_program('bash')
python3 = find_program('python3')
nasm = find_program('nasm')
objcopy = find_program('objcopy')
git = find_program('git', required: false)
find_program('grub-mkrescue')

# ------------------------------------------------------------------------------
# Build metadata (mirrors the Makefile's VERSION_DEFS)
# ------------------------------------------------------------------------------

cact_version = fs.exists('VERSION') ? fs.read('VERSION').strip() : 'unknown'

cact_commit = 'no-git'
if git.found()
  _rev = run_command(git, 'rev-parse', '--short', 'HEAD', check: false)
  if _rev.returncode() == 0
    cact_commit = _rev.stdout().strip()
  endif
endif

cact_build_time = 'unknown'
_date = run_command('date', '+%Y-%m-%d %H:%M:%S', check: false)
if _date.returncode() == 0
  cact_build_time = _date.stdout().strip()
endif

# ------------------------------------------------------------------------------
# Include paths — same list, same order as the Makefile's CFLAGS
#
# The Makefile added -I$(KERN_SC_*_DIR) per object file; they are global here
# because Meson has no per-source include directories.
#
# Each library is therefore built with implicit_include_directories: false.
# Meson otherwise prepends the directory of the source file, which would shadow
# headers the kernel deliberately picks through include order: "net.h" exists
# both as Cact/net/net.h and as syscall/sock/net.h, and sock/net.c wants the
# former for <net.h> and the latter for "net.h" — the split the Makefile's -I
# ordering produced.
# ------------------------------------------------------------------------------

cact_includes = include_directories(
  'Cact/kernel/core',
  'Cact/kernel/core/kern_ver',
  'Cact/kernel/core/syscall',
  'Cact/kernel/gdt',
  'Cact/kernel/cpudev',
  'Cact/kernel/energy',
  'Cact/kernel/smp',
  'Cact/kernel/elf',
  'Cact/kernel/memory',
  'Cact/kernel/proc',
  'Cact/kernel/sync',
  'Cact/kernel/idt',
  'Cact/drivers/input',
  'Cact/drivers/pci',
  'Cact/drivers/pci/enum',
  'Cact/drivers/pci/driver',
  'Cact/drivers/pci/loader',
  'Cact/drivers/initfs',
  'Cact/drivers/pci/msi',
  'Cact/drivers/pci/pcie',
  'Cact/drivers/block/blkdev',
  'Cact/drivers/usb',
  'Cact/drivers/usb/xhci',
  'Cact/drivers/usb/hid',
  'Cact/drivers/usb/hub',
  'Cact/drivers/quirks/acpi',
  'Cact/drivers/quirks/xhci',
  'Cact/drivers/timer',
  'Cact/fs/vfs',
  'Cact/fs/pipe',
  'Cact/fs/memfd',
  'Cact/fs/eventfd',
  'Cact/fs/timerfd',
  'Cact/fs/signalfd',
  'Cact/fs/epoll',
  'Cact/fs/pty',
  'Cact/drivers/tty',
  'Cact/fs/vfs/devfs',
  'Cact/drivers/block/pagecache',
  'Cact/fs/vfs/procfs',
  'Cact/fs/vfs/mntfs',
  'Cact/fs/vfs/vfsdev',
  'Cact/fs/vfs/etcfs',
  'Cact/fs/vfs/tmpfs',
  'Cact/fs/vfs/binfs',
  'Cact/fs/vfs/sbinfs',
  'Cact/fs/vfs/libfs',
  'Cact/fs/vfs/varfs',
  'Cact/fs/vfs/usrfs',
  'Cact/net',
  'Cact/net/arp',
  'Cact/net/ethernet',
  'Cact/net/ip',
  'Cact/net/icmp',
  'Cact/net/protocols/udp',
  'Cact/net/protocols/tcp',
  'Cact/net/socket',
  'Cact/drivers/video/fb',
  'Cact/drivers/video/font',
  'Cact/drivers/gpu/drm',
  'Cact/drivers/gpu/drm/uapi',
  'Cact/drivers/acpi',
  'Cact/drivers/acpi/include',
  'Cact/drivers/acpi/acpica/source/include',
  'Cact/kernel/core/syscall/process',
  'Cact/kernel/core/syscall/io',
  'Cact/kernel/core/syscall/mem',
  'Cact/kernel/core/syscall/sock',
  'Cact/kernel/core/syscall/module',
)

kernel_c_args = [
  '-ffreestanding',
  '-fno-pie',
  '-fno-stack-protector',
  '-nostdlib',
  # Kernel code runs before (and without) CR4.OSFXSR being set — the FPU/SSE
  # state belongs to user processes — and the i686-cact Rust target is built
  # with the same -mmx,-sse.  clang otherwise copies structs with SSE2 (a
  # movsd in multiboot2_parse is enough) and dies with #UD -> #DF before the
  # IDT exists.  GCC happens not to emit SSE there, but the constraint is real.
  '-mno-mmx',
  '-mno-sse',
  '-mno-sse2',
  '-DCACT_ACPI_FREESTANDING',
  '-DCACT_VERSION=' + cact_version,
  '-DCACT_COMMIT_HASH=' + cact_commit,
  # version.c stringifies this with STR(), and the Makefile let the shell strip
  # the quotes, so the macro body must stay bare tokens to match its banner.
  '-DCACT_BUILD_TIME=' + cact_build_time,
]

if get_option('kern_debug')
  kernel_c_args += ['-g', '-Og']
endif

# ------------------------------------------------------------------------------
# Accumulators filled in by the per-directory meson.build files
# ------------------------------------------------------------------------------

kernel_objects = []      # raw objects: NASM stubs and the SMP trampoline
kernel_libs = []         # per-directory C archives, linked --whole-archive
kernel_rust_libs = []    # cargo archives, linked inside a --start-group

rust_build_script = src_root / 'tools/cact_rust_build.sh'

# ------------------------------------------------------------------------------
# Kernel
# ------------------------------------------------------------------------------

subdir('Cact/kernel/core')
subdir('Cact/kernel/core/kern_ver')
subdir('Cact/kernel/core/syscall')
subdir('Cact/kernel/core/syscall/process')
subdir('Cact/kernel/core/syscall/io')
subdir('Cact/kernel/core/syscall/mem')
subdir('Cact/kernel/core/syscall/sock')
subdir('Cact/kernel/core/syscall/module')
subdir('Cact/kernel/gdt')
subdir('Cact/kernel/idt')
subdir('Cact/kernel/idt/fpu')
subdir('Cact/kernel/cpudev')
subdir('Cact/kernel/elf')
subdir('Cact/kernel/memory')
subdir('Cact/kernel/proc')
subdir('Cact/kernel/smp')

# ------------------------------------------------------------------------------
# Filesystems
# ------------------------------------------------------------------------------

subdir('Cact/fs/vfs')
subdir('Cact/fs/vfs/devfs')
subdir('Cact/fs/vfs/procfs')
subdir('Cact/fs/vfs/mntfs')
subdir('Cact/fs/vfs/vfsdev')
subdir('Cact/fs/vfs/etcfs')
subdir('Cact/fs/vfs/tmpfs')
subdir('Cact/fs/vfs/binfs')
subdir('Cact/fs/vfs/sbinfs')
subdir('Cact/fs/vfs/libfs')
subdir('Cact/fs/vfs/varfs')
subdir('Cact/fs/vfs/usrfs')
subdir('Cact/fs/pipe')
subdir('Cact/fs/memfd')
subdir('Cact/fs/eventfd')
subdir('Cact/fs/timerfd')
subdir('Cact/fs/signalfd')
subdir('Cact/fs/epoll')
subdir('Cact/fs/pty')

# ------------------------------------------------------------------------------
# Drivers
# ------------------------------------------------------------------------------

subdir('Cact/drivers/acpi')
subdir('Cact/drivers/block/blkdev')
subdir('Cact/drivers/block/pagecache')
subdir('Cact/drivers/initfs')
subdir('Cact/drivers/input')
subdir('Cact/drivers/pci')
subdir('Cact/drivers/pci/enum')
subdir('Cact/drivers/pci/driver')
subdir('Cact/drivers/pci/loader')
subdir('Cact/drivers/pci/msi')
subdir('Cact/drivers/pci/pcie')
subdir('Cact/drivers/quirks/acpi')
subdir('Cact/drivers/quirks/xhci')
subdir('Cact/drivers/timer')
subdir('Cact/drivers/tty')
subdir('Cact/drivers/usb')
subdir('Cact/drivers/usb/hid')
subdir('Cact/drivers/usb/hub')
subdir('Cact/drivers/usb/xhci')
subdir('Cact/drivers/video/fb')
subdir('Cact/drivers/video/font')
subdir('Cact/drivers/gpu')

# ------------------------------------------------------------------------------
# Rust
# ------------------------------------------------------------------------------

subdir('Cact/kernel/proc/sched')
subdir('Cact/kernel/memory/rust_mm')
subdir('Cact/net/rust_net')
subdir('Cact/drivers/gpu/drm/rust_drm')
# The one Rust staticlib (all other kernel Rust crates are rlibs).
subdir('Cact/kernel/kernel_rust')
subdir('Cact/crypto/hmac_ffi')

# ------------------------------------------------------------------------------
# Final link
# ------------------------------------------------------------------------------

if get_option('linker') == 'lld'
  ld = find_program('ld.lld', required: false)
  if not ld.found()
    ld = find_program('ld')
  endif
else
  ld = find_program('ld')
endif

kernel_bin = custom_target('kernel-bin',
  output: 'kernel.bin',
  input: kernel_objects,
  command: [ld, '-m', 'elf_i386', '-T', src_root / 'linker.ld',
            '-z', 'noexecstack', '-o', '@OUTPUT@'] +
           kernel_objects +
           ['--whole-archive'] + kernel_libs + ['--no-whole-archive'] +
           ['--start-group'] + kernel_rust_libs + ['--end-group'],
  depend_files: files('linker.ld'),
  build_by_default: true)

# ------------------------------------------------------------------------------
# Bootable ISO images
# ------------------------------------------------------------------------------

iso_script = src_root / 'tools/cact_make_iso.sh'
isodir = build_root / 'isodir'

if get_option('local_repo') == ''
  local_repo = src_root / '..' / 'LocalRepoCactOS'
else
  # A relative value is relative to the source root, matching the default
  # above: run_targets execute from the build directory, so a bare
  # "../LocalRepoCactOS-x86_32" would otherwise resolve one level too deep.
  _lr = get_option('local_repo')
  local_repo = _lr.startswith('/') ? _lr : src_root / _lr
endif
cctkfs_img = local_repo / 'cctkfs.img'

iso_inputs = [kernel_bin]
iso_inputs += files(get_option('iso_grub_cfg'))

cact_iso = custom_target('cact-iso',
  output: 'cact.iso',
  input: iso_inputs,
  command: [bash, iso_script, '@INPUT0@', '@INPUT1@', isodir, '@OUTPUT@'],
  build_by_default: true)

# Unlike the kernel-only image this one depends on an artifact living outside
# the build tree (cctkfs.img), so it is a run_target: always rebuilt, and it
# fails loudly when the image is missing rather than silently reusing a stale
# cact-full.iso.
run_target('iso-full',
  command: [bash, iso_script, kernel_bin, src_root / 'grub.cfg',
            build_root / 'isodir-full', build_root / 'cact-full.iso', cctkfs_img])

# ------------------------------------------------------------------------------
# Housekeeping targets (make clean / acpica-clean / acpica-update)
# ------------------------------------------------------------------------------

rust_crate_dirs = [
  src_root / 'Cact/kernel/proc/sched',
  src_root / 'Cact/kernel/memory/rust_mm',
  src_root / 'Cact/net/rust_net',
  src_root / 'Cact/crypto/hmac_ffi',
]

run_target('rust-clean',
  command: [bash, '-c', 'for d in ' + ' '.join(rust_crate_dirs) +
            '; do (cd "$d" && cargo clean); done'])

run_target('acpica-clean',
  command: [find_program('rm'), '-rf', src_root / 'Cact/drivers/acpi/acpica'])

if git.found()
  run_target('acpica-update',
    command: [git, '-C', src_root / 'Cact/drivers/acpi/acpica', 'pull'])
endif

message('')
message('  Cact kernel ' + cact_version + ' (' + cact_commit + ')')
message('  compiler: ' + cc.get_id() + ' ' + cc.version())
message('  linker  : ' + ld.full_path())
message('')
message('  ninja -C build-meson          -> kernel.bin + cact.iso')
message('  ninja -C build-meson iso-full -> cact-full.iso')
message('')
