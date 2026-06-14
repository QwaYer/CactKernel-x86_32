#!/usr/bin/env python3
"""cact_sign.py — Sign .cctk ELF module with HMAC-SHA256.

Appends a 32-byte HMAC-SHA256 tag to the module file.
The tag is computed as HMAC-SHA256(key, ELF data).

Key is read from Cact/crypto/hmac_ffi/hmac_key.bin
(relative to the project root, resolved from this script's location).
Generate with: python3 tools/gen_hmac_key.py
"""

import os
import sys
import hmac
import hashlib


def _key_path() -> str:
    """Resolve hmac_key.bin relative to the project root."""
    script_dir = os.path.dirname(os.path.abspath(__file__))
    return os.path.join(script_dir, "..", "Cact", "crypto", "hmac_ffi", "hmac_key.bin")


def _load_key() -> bytes:
    path = _key_path()
    try:
        with open(path, "rb") as f:
            key = f.read()
    except FileNotFoundError:
        print(f"HMAC key not found at {path}", file=sys.stderr)
        print("Generate one with: python3 tools/gen_hmac_key.py", file=sys.stderr)
        sys.exit(1)
    if len(key) != 32:
        print(f"HMAC key must be exactly 32 bytes, got {len(key)}", file=sys.stderr)
        sys.exit(1)
    return key


def sign(data: bytes) -> bytes:
    return hmac.new(_load_key(), data, hashlib.sha256).digest()


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
