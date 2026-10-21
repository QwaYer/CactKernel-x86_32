#!/usr/bin/env python3
"""cact_sign.py — Sign a .cctk module with an ECDSA P-256 signature.

Appends the module trailer (see tools/modsign.py):

    [ ELF ][ magic:4 = "CMOD" ][ vermagic:4 LE ][ signature:64 ]

The private key lives at Cact/crypto/modsign/module_sign_priv.pem; the kernel
embeds only the public key and verifies the signature plus the vermagic ABI
fingerprint.  Idempotent per ABI: a module already signed for the current
vermagic is left unchanged, while one carrying an older vermagic (signed before
ksym.c changed) is stripped and re-signed, since the kernel would refuse it.
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

    state = modsign.sign_state(data)
    vm = modsign.vermagic()
    if state == "current":
        print(f"cact_sign: {path} — already signed, vermagic=0x{vm:08x}")
        sys.exit(0)

    was = modsign.trailer_vermagic(data)

    if not os.path.isfile(modsign.PRIV_PEM):
        modsign.generate_keys()

    signed = modsign.sign_module(modsign.PRIV_PEM, data)
    with open(path, "wb") as f:
        f.write(signed)

    if state == "stale":
        print(f"cact_sign: {path} — re-signed: vermagic 0x{was:08x} -> 0x{vm:08x}")
    else:
        print(f"signed: {path}  ECDSA P-256, vermagic=0x{vm:08x}")
    sys.exit(0)


if __name__ == "__main__":
    main()
