#!/usr/bin/env python3
"""cact_sign.py — Sign .cctk ELF module with HMAC-SHA256.

Appends a 32-byte HMAC-SHA256 tag to the module file.
The tag is computed as HMAC-SHA256(key, ELF data).

Must match the key in cact_crypto/src/hmac_ffi.rs.
"""

import sys
import hmac
import hashlib

CACT_HMAC_KEY = b"CactKernel-HMAC-Secret-2026-32B!!"

def sign(data: bytes) -> bytes:
    return hmac.new(CACT_HMAC_KEY, data, hashlib.sha256).digest()

def main():
    if len(sys.argv) != 2:
        print(f"Usage: {sys.argv[0]} <module.cctk>", file=sys.stderr)
        sys.exit(1)

    path = sys.argv[1]
    with open(path, "rb") as f:
        elf_data = f.read()

    tag = sign(elf_data)

    with open(path, "ab") as f:
        f.write(tag)

    print(f"signed: {path}  tag={tag.hex()}")
    sys.exit(0)

if __name__ == "__main__":
    main()
