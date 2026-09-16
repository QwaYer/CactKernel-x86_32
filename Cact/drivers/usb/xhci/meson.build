kernel_libs += static_library('cact_xhci',
  files(
    'xhci.c',
    'xhci_hw.c',
    'xhci_ring.c',
    'xhci_dev.c',
    'xhci_xfer.c',
  ),
  include_directories: cact_includes,
  implicit_include_directories: false,
  c_args: kernel_c_args)
