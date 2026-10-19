# ==============================================================================
# Cact/drivers/zstd — in-kernel zstd decompressor
# ==============================================================================
#
# Imported from Linux 7.3.0-rc4 (decompress-only object set):
#   lib/zstd/{common,decompress}  — from lib/zstd/Makefile (zstd_decompress +
#                                   zstd_common)
#   lib/xxhash.c                  — zstd's only source dependency
#   include/linux/zstd*.h, xxhash.h
#
# The imported sources are unmodified apart from their includes, which point at
# the Cact compat headers in compat/ instead of <linux/...>.  cact_zstd.c is the
# Cact glue: single-frame decompression into an aligned kmalloc'd DCtx
# workspace.
# ==============================================================================

zstd_includes = include_directories('.', 'common', 'decompress')

kernel_libs += static_library('cact_zstd',
  files(
    'common/debug.c',
    'common/entropy_common.c',
    'common/error_private.c',
    'common/fse_decompress.c',
    'common/zstd_common.c',
    'decompress/huf_decompress.c',
    'decompress/zstd_ddict.c',
    'decompress/zstd_decompress.c',
    'decompress/zstd_decompress_block.c',
    'xxhash.c',
    'cact_zstd.c',
  ),
  include_directories: [zstd_includes, cact_includes],
  implicit_include_directories: false,
  c_args: kernel_c_args)
