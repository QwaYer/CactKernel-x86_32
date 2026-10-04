# `cact_vfs` is the Rust core of the VFS: path resolution, the dentry cache,
# the mount tree, symlinks and node lifetime.  It is an rlib; the kernel's
# single Rust archive is built by `Cact/kernel/kernel_rust` (see its src/lib.rs).
#
# C filesystems and drivers keep registering through the C `vfs_ops_t` /
# `register_chrdev` ABI and are untouched by this crate.
_rust_inputs = []
_found = run_command('sh', '-c',
  'find ' + meson.current_source_dir() +
  ' \\( -name target -o -name vendor \\) -prune -o -type f -print | LC_ALL=C sort',
  check: true)

foreach f : _found.stdout().strip().split('\n')
  if f != ''
    _rust_inputs += f
  endif
endforeach
