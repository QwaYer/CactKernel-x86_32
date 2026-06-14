#!/usr/bin/env python3
"""gen_hmac_key.py — Generate a random 32-byte HMAC key for module signing.

Writes to Cact/crypto/hmac_ffi/hmac_key.bin (relative to project root).
If the file already exists, it is NOT overwritten unless --force is passed.
"""

import os
import sys
import stat

KEY_PATH = os.path.join(
    os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
    "Cact", "crypto", "hmac_ffi", "hmac_key.bin"
)

def main():
    force = "--force" in sys.argv

    if os.path.exists(KEY_PATH) and not force:
        print(f"Key already exists: {KEY_PATH}")
        print("Use --force to overwrite.")
        sys.exit(0)

    key = os.urandom(32)

    os.makedirs(os.path.dirname(KEY_PATH), exist_ok=True)
    with open(KEY_PATH, "wb") as f:
        f.write(key)

    # Remove all permissions for group/other
    os.chmod(KEY_PATH, stat.S_IRUSR | stat.S_IWUSR)

    print(f"Generated 32-byte HMAC key: {KEY_PATH}")
    print(f"Key (hex): {key.hex()}")
    sys.exit(0)

if __name__ == "__main__":
    main()
