#!/usr/bin/env python3
"""cact_sign_cctkfs.py — Sign all .cctk modules inside a cctkfs archive.

Reads cctkfs.img, signs each module data blob with HMAC-SHA256,
rebuilds the archive with properly aligned signed blobs,
and writes a CRC-32 container checksum into the header.

Key is read from Cact/crypto/hmac_ffi/hmac_key.bin
(relative to the project root, resolved from this script's location).
Generate with: python3 tools/gen_hmac_key.py
"""

import os
import sys
import struct
import hmac
import hashlib
import zlib

CCTKFS_MAGIC  = 0x53464B43
CCTKFS_CKSUM_OFF = 28
TAG_SIZE      = 32


def _key_path() -> str:
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


CACT_HMAC_KEY = _load_key()


def hmac_sign(data: bytes) -> bytes:
    return hmac.new(CACT_HMAC_KEY, data, hashlib.sha256).digest()


def align_up(val: int, align: int) -> int:
    return (val + align - 1) & ~(align - 1)


def set_checksum(data: bytearray) -> None:
    data[CCTKFS_CKSUM_OFF:CCTKFS_CKSUM_OFF + 4] = b'\x00\x00\x00\x00'
    crc = zlib.crc32(bytes(data)) & 0xFFFFFFFF
    struct.pack_into("<I", data, CCTKFS_CKSUM_OFF, crc)


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

    HEADER_SIZE  = 32
    ENTRY_SIZE   = 24
    NAME_ALIGN   = 8
    DATA_ALIGN   = 16

    new_entries_off = HEADER_SIZE
    new_names_off   = new_entries_off + count * ENTRY_SIZE
    new_names_off   = align_up(new_names_off, NAME_ALIGN)

    name_blob = img[names_off : names_off + names_size]
    new_names_size = names_size
    new_data_off = new_names_off + new_names_size
    new_data_off = align_up(new_data_off, DATA_ALIGN)

    new_data_blobs = []
    for ent in entries:
        blob = ent["data"]
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

    out = bytearray()
    out.extend(struct.pack("<IIIIIIII",
        CCTKFS_MAGIC, 1, 0,
        count, HEADER_SIZE, new_names_off, new_names_size, 0))

    cur_data_off = new_data_off
    for i, ent in enumerate(entries):
        data_size = len(new_data_blobs[i])
        out.extend(struct.pack("<IIIIII",
            ent["name_off"], ent["name_len"],
            cur_data_off, data_size, ent["flags"], 0))
        cur_data_off += data_size
        cur_data_off = align_up(cur_data_off, DATA_ALIGN)

    out.extend(name_blob)
    while len(out) % DATA_ALIGN != 0:
        out.append(0)

    for blob in new_data_blobs:
        out.extend(blob)
        while len(out) % DATA_ALIGN != 0:
            out.append(0)

    total_size = len(out)
    struct.pack_into("<I", out, 8, total_size)

    set_checksum(out)

    with open(path, "wb") as f:
        f.write(out)

    ck = out[CCTKFS_CKSUM_OFF:CCTKFS_CKSUM_OFF + 4]
    print(f"cctkfs: done — {count} modules signed, "
          f"total_size={total_size}, "
          f"crc32=0x{ck.hex()}")
    sys.exit(0)


if __name__ == "__main__":
    main()
