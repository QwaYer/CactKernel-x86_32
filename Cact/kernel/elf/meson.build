# mod_tag.c includes mod_pubkey.h (the ECDSA public key + a known-answer
# signature), derived from the private key.  Both are gitignored, so generate
# the pair together whenever either is missing (a fresh checkout has neither).
_modsign_priv = src_root / 'Cact' / 'crypto' / 'modsign' / 'module_sign_priv.pem'
if not fs.exists(meson.current_source_dir() / 'mod_pubkey.h') or not fs.exists(_modsign_priv)
  run_command(python3, src_root / 'tools/gen_module_keys.py', check: true)
endif

kernel_libs += static_library('cact_elf',
  files(
    'elf_loader.c',
    'ksym.c',
    'mod_tag.c',
    'sym.c',
  ),
  include_directories: cact_includes,
  implicit_include_directories: false,
  c_args: kernel_c_args)
