#!/usr/bin/env python3
"""cact_sign_cctkfs.py — Sign all .cctk modules inside a cctkfs archive.

Reads cctkfs.img, signs each module data blob with HMAC-SHA256,
rebuilds the archive with properly aligned signed blobs.

Must match the key in cact_crypto/src/hmac_ffi.rs.
"""

import sys
import struct
import hmac
import hashlib

CACT_HMAC_KEY = b"CactKernel-HMAC-Secret-2026-32B!!"
CCTKFS_MAGIC  = 0x53464B43
TAG_SIZE      = 32

def hmac_sign(data: bytes) -> bytes:
    return hmac.new(CACT_HMAC_KEY, data, hashlib.sha256).digest()

def align_up(val: int, align: int) -> int:
    return (val + align - 1) & ~(align - 1)

def main():
    if len(sys.argv) != 2:
        print(f"Usage: {sys.argv[0]} <cctkfs.img>", file=sys.stderr)
        sys.exit(1)

    path = sys.argv[1]

    with open(path, "rb") as f:
        img = f.read()

    if len(img) < 32:
        print("cctkfs: file too small", file=sys.stderr)
        sys.exit(1)

    magic, version, total_size, count, entries_off, names_off, names_size, _ = \
        struct.unpack_from("<IIIIIIII", img, 0)

    if magic != CCTKFS_MAGIC:
        print(f"cctkfs: bad magic 0x{magic:08X}", file=sys.stderr)
        sys.exit(1)

    print(f"cctkfs: {count} modules, {total_size} bytes")

    # Parse entries
    entries = []
    for i in range(count):
        off = entries_off + i * 24
        name_off, name_len, data_off, data_size, flags, _ = \
            struct.unpack_from("<IIIIII", img, off)

        name_bytes = img[names_off + name_off : names_off + name_off + name_len]
        name = name_bytes.decode("utf-8", errors="replace")

        data = img[data_off : data_off + data_size]
        entries.append({
            "name_off": name_off,
            "name_len": name_len,
            "name": name,
            "data_off": data_off,
            "data_size": data_size,
            "flags": flags,
            "data": data,
        })

    # Rebuild archive
    HEADER_SIZE  = 32
    ENTRY_SIZE   = 24
    NAME_ALIGN   = 8
    DATA_ALIGN   = 16

    new_entries_off = HEADER_SIZE
    new_names_off   = new_entries_off + count * ENTRY_SIZE
    new_names_off   = align_up(new_names_off, NAME_ALIGN)

    # Build name blob (same as original)
    name_blob = img[names_off : names_off + names_size]
    new_names_size = names_size
    new_data_off = new_names_off + new_names_size
    new_data_off = align_up(new_data_off, DATA_ALIGN)

    # Sign data blobs
    new_data_blobs = []
    for ent in entries:
        blob = ent["data"]
        # If already exactly signed (ends with valid tag), detect and skip
        if len(blob) >= TAG_SIZE:
            elf_data = blob[:-TAG_SIZE]
            stored_tag = blob[-TAG_SIZE:]
            if hmac_sign(elf_data) == stored_tag:
                print(f"  [{ent['name']}]: already signed, OK")
                new_data_blobs.append(blob)
                continue

        signed = blob + hmac_sign(blob)
        new_data_blobs.append(signed)
        print(f"  [{ent['name']}]: signed tag={hmac_sign(blob).hex()}")

    # Write output
    out = bytearray()
    out.extend(struct.pack("<IIIIIIII",
        CCTKFS_MAGIC, 1, 0,  # magic, version, total_size placeholder
        count, HEADER_SIZE, new_names_off, new_names_size, 0))

    # Entries
    cur_data_off = new_data_off
    for i, ent in enumerate(entries):
        data_size = len(new_data_blobs[i])
        out.extend(struct.pack("<IIIIII",
            ent["name_off"], ent["name_len"],
            cur_data_off, data_size, ent["flags"], 0))
        cur_data_off += data_size
        cur_data_off = align_up(cur_data_off, DATA_ALIGN)

    # Name blob
    out.extend(name_blob)
    # Pad to DATA_ALIGN
    while len(out) % DATA_ALIGN != 0:
        out.append(0)

    # Data blobs (each 16-byte aligned)
    for blob in new_data_blobs:
        out.extend(blob)
        while len(out) % DATA_ALIGN != 0:
            out.append(0)

    total_size = len(out)
    struct.pack_into("<I", out, 8, total_size)

    with open(path, "wb") as f:
        f.write(out)

    print(f"cctkfs: done — {count} modules signed, total_size={total_size}")
    sys.exit(0)

if __name__ == "__main__":
    main()
