kernel_libs += static_library('cact_pci_msi',
  files(
    'msidev.c',   # vector pool, IDT stubs, dispatch, msidev_register()
    'msix.c',     # MSI-X capability (ID 0x11)
    'msi.c',      # MSI capability (ID 0x05)
  ),
  include_directories: cact_includes,
  implicit_include_directories: false,
  c_args: kernel_c_args)
