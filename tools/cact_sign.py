#!/usr/bin/env python3
"""cact_sign.py — Sign a .cctk module with an ECDSA P-256 signature.

Appends the module trailer (see tools/modsign.py):

    [ ELF ][ magic:4 = "CMOD" ][ vermagic:4 LE ][ signature:64 ]

The private key lives at Cact/crypto/modsign/module_sign_priv.pem; the kernel
embeds only the public key and verifies the signature plus the vermagic ABI
fingerprint.  Idempotent: an already-signed module is left unchanged.
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import modsign  # noqa: E402


def main():
    if len(sys.argv) != 2:
        print(f"Usage: {sys.argv[0]} <module.cctk>", file=sys.stderr)
        sys.exit(1)

    path = sys.argv[1]
    with open(path, "rb") as f:
        data = f.read()

    if modsign.already_signed(data):
        print(f"cact_sign: {path} — already signed")
        sys.exit(0)

    if not os.path.isfile(modsign.PRIV_PEM):
        modsign.generate_keys()

    signed = modsign.sign_module(modsign.PRIV_PEM, data)
    with open(path, "wb") as f:
        f.write(signed)

    print(f"signed: {path}  ECDSA P-256, vermagic=0x{modsign.vermagic():08x}")
    sys.exit(0)


if __name__ == "__main__":
    main()
