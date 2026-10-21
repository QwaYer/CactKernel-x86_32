#!/usr/bin/env python3
"""gen_module_keys.py — Generate the ECDSA P-256 module-signing key pair.

Creates (if missing) the private key at
Cact/crypto/modsign/module_sign_priv.pem and regenerates the kernel's public-key
header from it (public point, a known-answer signature, and a signed sample
module for the boot self-test).  --force rotates the key.  An optional path
argument redirects the header, which is how the kernel build regenerates it into
its build directory; the default lands next to the key, off every include path.

The private key signs modules at build time and must NOT be embedded in the
kernel image; only the public key is compiled in.
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import modsign  # noqa: E402


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    force = "--force" in sys.argv
    out = args[0] if args else modsign.PUB_HEADER
    priv = modsign.generate_keys(force=force, out=out)
    print("module signing key:", priv)
    print("public key header: ", out)
    print("vermagic:          ", "0x%08x" % modsign.vermagic())


if __name__ == "__main__":
    main()
