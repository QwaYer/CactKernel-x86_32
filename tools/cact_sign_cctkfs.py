#!/usr/bin/env python3
"""cact_sign_cctkfs.py — Sign all .cctk modules inside a cctkfs archive.

Reads cctkfs.img, signs each module data blob with an ECDSA P-256 signature
(see tools/modsign.py), rebuilds the archive with properly aligned signed
blobs, and writes a CRC-32 container checksum into the header.

The private key is Cact/crypto/modsign/module_sign_priv.pem; the kernel embeds
only the public key.  Idempotent: already-signed blobs are left unchanged.
"""

import os
import sys
import struct
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import modsign  # noqa: E402

CCTKFS_MAGIC  = 0x53464B43
CCTKFS_CKSUM_OFF = 28


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

    if not os.path.isfile(modsign.PRIV_PEM):
        modsign.generate_keys()

    new_data_blobs = []
    for ent in entries:
        blob = ent["data"]
        if modsign.already_signed(blob):
            print(f"  [{ent['name']}]: already signed, OK")
            new_data_blobs.append(blob)
            continue

        signed = modsign.sign_module(modsign.PRIV_PEM, blob)
        new_data_blobs.append(signed)
        print(f"  [{ent['name']}]: signed (ECDSA P-256)")

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
