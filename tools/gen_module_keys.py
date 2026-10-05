#!/usr/bin/env python3
"""gen_module_keys.py — Generate the ECDSA P-256 module-signing key pair.

Creates (if missing) the private key at
Cact/crypto/modsign/module_sign_priv.pem and always regenerates the kernel's
public-key header Cact/kernel/elf/mod_pubkey.h from it (public point + a
known-answer signature for the boot self-test).  --force rotates the key.

The private key signs modules at build time and must NOT be embedded in the
kernel image; only the public key is compiled in.
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import modsign  # noqa: E402


def main():
    force = "--force" in sys.argv
    priv = modsign.generate_keys(force=force)
    print("module signing key:", priv)
    print("public key header: ", modsign.PUB_HEADER)
    print("vermagic:          ", "0x%08x" % modsign.vermagic())


if __name__ == "__main__":
    main()
