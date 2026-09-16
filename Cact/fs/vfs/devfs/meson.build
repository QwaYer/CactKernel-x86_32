kernel_libs += static_library('cact_devfs',
  files(
    'devfs.c',
    'devfs_devices.c',
    'devfs_services.c',
    'devfs_crypto.c',
  ),
  include_directories: cact_includes,
  implicit_include_directories: false,
  c_args: kernel_c_args)
