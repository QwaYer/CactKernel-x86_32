# mod_tag.c includes the generated mod_pubkey_gen.h: the ECDSA public key that
# verifies every loadable module.  The header derives from the private key
# alone, so it is regenerated when the key pair changes.
#
# The bootstrap below only creates the key pair when it is missing: the key is
# an input of the header target, so meson has to find it at configure time.
_modsign_priv = src_root / 'Cact' / 'crypto' / 'modsign' / 'module_sign_priv.pem'
if not fs.exists(_modsign_priv)
  run_command(python3, src_root / 'tools/gen_module_keys.py', check: true)
endif

_modsign_inputs = files('ksym.c')
if fs.exists(_modsign_priv)
  _modsign_inputs += files('../../crypto/modsign/module_sign_priv.pem')
endif

mod_pubkey_h = custom_target('mod_pubkey_gen.h',
  input: _modsign_inputs,
  output: 'mod_pubkey_gen.h',
  command: [python3, src_root / 'tools' / 'gen_module_keys.py', '@OUTPUT@'])

kernel_libs += static_library('cact_elf',
  files(
    'elf_loader.c',
    'ksym.c',
    'mod_tag.c',
    'sym.c',
  ) + mod_pubkey_h,
  include_directories: cact_includes,
  implicit_include_directories: false,
  c_args: kernel_c_args)
