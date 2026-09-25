kernel_libs += static_library('cact_syscall_process',
  files(
    'proc.c',
    'signal.c',
    'task_comm.c',
  ),
  include_directories: cact_includes,
  implicit_include_directories: false,
  c_args: kernel_c_args)
