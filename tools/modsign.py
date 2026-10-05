#!/usr/bin/env python3
"""modsign.py — CactOS loadable-module signing (vermagic + ECDSA P-256).

Shared library and CLI used by cact_sign.py, cact_sign_cctkfs.py and
gen_module_keys.py.

* vermagic() reproduces the kernel's ksym_vermagic(): a 32-bit FNV-1a hash over
  the sorted exported symbol names in Cact/kernel/elf/ksym.c.  cact_check.sh
  compares the two at boot, so this and the C routine must stay in lockstep.

* The signature is ECDSA over SHA-256 with a P-256 key, in fixed 64-byte r||s
  form (the DER produced by OpenSSL is converted).  Signing uses the OpenSSL CLI
  so the build host needs no Python crypto package; the kernel verifies with its
  own cact_sig_verify_p256_raw(), holding only the public key.

Module trailer (see Cact/kernel/elf/mod_tag.h):

    [ ELF ][ magic:4 = "CMOD" ][ vermagic:4 LE ][ signature:64 ]

The signature covers ELF || magic || vermagic.
"""

import os
import re
import struct
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
KSYM_C = os.path.join(ROOT, "Cact", "kernel", "elf", "ksym.c")
MODSIGN_DIR = os.path.join(ROOT, "Cact", "crypto", "modsign")
PRIV_PEM = os.path.join(MODSIGN_DIR, "module_sign_priv.pem")
PUB_HEADER = os.path.join(ROOT, "Cact", "kernel", "elf", "mod_pubkey.h")

MAGIC = b"CMOD"
SIG_SIZE = 64
TRAILER_SIZE = len(MAGIC) + 4 + SIG_SIZE

SELFTEST_MSG = b"CactOS module-signing self-test v1"

FNV_OFFSET = 2166136261
FNV_PRIME = 16777619
VERMAGIC_SEED = b"CACT-MODVER-1\x00"

_ENTRY_RE = re.compile(r'\s*\{\s*"([^"]+)"\s*,')


def ksym_names(path=KSYM_C):
    """Exported symbol names parsed from ksym.c, in table order."""
    names = []
    with open(path) as f:
        for line in f:
            m = _ENTRY_RE.match(line)
            if m:
                names.append(m.group(1))
    return names


def vermagic(path=KSYM_C):
    """The kernel's ksym_vermagic() value for the given ksym.c."""
    h = FNV_OFFSET

    def feed(bs):
        nonlocal h
        for b in bs:
            h ^= b
            h = (h * FNV_PRIME) & 0xFFFFFFFF

    feed(VERMAGIC_SEED)
    for name in sorted(ksym_names(path)):
        feed(name.encode("ascii"))
        feed(b"\x00")
    return h


def _openssl(*args, input=None):
    return subprocess.run(["openssl", *args], input=input,
                          stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                          check=True)


def _der_to_raw(der):
    """ECDSA DER SEQUENCE(r,s) -> fixed 64-byte r||s (P-256)."""
    if len(der) < 8 or der[0] != 0x30:
        raise ValueError("not an ECDSA DER signature")
    i = 2
    if der[1] & 0x80:                       # long-form length
        i = 2 + (der[1] & 0x7F)
    if der[i] != 0x02:
        raise ValueError("bad DER INTEGER (r)")
    lr = der[i + 1]
    r = der[i + 2:i + 2 + lr]
    i = i + 2 + lr
    if der[i] != 0x02:
        raise ValueError("bad DER INTEGER (s)")
    ls = der[i + 1]
    s = der[i + 2:i + 2 + ls]
    return (int.from_bytes(r, "big").to_bytes(32, "big") +
            int.from_bytes(s, "big").to_bytes(32, "big"))


def sign_raw(priv_pem, msg):
    """64-byte ECDSA P-256 signature over SHA-256(msg)."""
    der = _openssl("dgst", "-sha256", "-sign", priv_pem, input=msg).stdout
    return _der_to_raw(der)


def sign_module(priv_pem, elf):
    """Append the module trailer (magic|vermagic|signature) to an ELF image."""
    from struct import pack
    vm = vermagic()
    signed = elf + MAGIC + pack("<I", vm)
    return signed + sign_raw(priv_pem, signed)


def already_signed(data, priv_pem=None):
    """True when `data` already carries our trailer (magic present).

    A magic match is a reliable enough signal that the file was signed by this
    scheme; re-signing an already-signed blob would stack trailers.
    """
    return len(data) >= TRAILER_SIZE and data[-TRAILER_SIZE:-TRAILER_SIZE + 4] == MAGIC


def strip_trailer(data):
    """Drop a trailer this scheme added; leave the ELF image."""
    if already_signed(data):
        return data[:-TRAILER_SIZE]
    return data


def _pub_point(priv_pem):
    der = _openssl("ec", "-in", priv_pem, "-pubout", "-outform", "DER",
                   "-conv_form", "uncompressed").stdout
    point = der[-65:]
    if len(point) != 65 or point[0] != 0x04:
        raise ValueError("could not extract an uncompressed P-256 public point")
    return point


def _c_array(data, indent="    "):
    lines = []
    for i in range(0, len(data), 12):
        chunk = ", ".join("0x%02x" % b for b in data[i:i + 12])
        lines.append(indent + chunk + ",")
    return "\n".join(lines)


def write_public_header(priv_pem=PRIV_PEM, out=PUB_HEADER):
    point = _pub_point(priv_pem)
    sig = sign_raw(priv_pem, SELFTEST_MSG)
    # A synthetic signed "module" (arbitrary payload + real trailer) lets the
    # kernel boot self-test exercise the full mod_tag_verify() path — magic,
    # signature and vermagic — without needing real hardware to load a .cctk.
    payload = b"CactOS module-trailer self-test payload v1"
    blob = sign_module(priv_pem, payload)
    with open(out, "w") as f:
        f.write(
            "/* Generated by tools/gen_module_keys.py — do not edit.\n"
            " * ECDSA P-256 public key + known-answer signature used to verify\n"
            " * loadable modules (see mod_tag.c). */\n"
            "#ifndef CACT_MOD_PUBKEY_H\n"
            "#define CACT_MOD_PUBKEY_H\n"
            "#include <stdint.h>\n\n"
            "#define CACT_MODULE_PUBKEY_LEN %d\n"
            "static const uint8_t cact_module_pubkey[CACT_MODULE_PUBKEY_LEN] = {\n%s\n};\n\n"
            "#define CACT_MODSIGN_SELFTEST_MSG_LEN %d\n"
            "static const uint8_t cact_modsign_selftest_msg[CACT_MODSIGN_SELFTEST_MSG_LEN] = {\n%s\n};\n"
            "static const uint8_t cact_modsign_selftest_sig[%d] = {\n%s\n};\n\n"
            "#define CACT_MODSIGN_SELFTEST_MOD_LEN %d\n"
            "#define CACT_MODSIGN_SELFTEST_MOD_TOTAL %d\n"
            "static const uint8_t cact_modsign_selftest_mod[CACT_MODSIGN_SELFTEST_MOD_TOTAL] = {\n%s\n};\n\n"
            "#endif /* CACT_MOD_PUBKEY_H */\n"
            % (len(point), _c_array(point),
               len(SELFTEST_MSG), _c_array(SELFTEST_MSG),
               len(sig), _c_array(sig),
               len(payload), len(blob), _c_array(blob)))
    return out


def generate_keys(force=False):
    os.makedirs(MODSIGN_DIR, exist_ok=True)
    if force or not os.path.isfile(PRIV_PEM):
        _openssl("ecparam", "-name", "prime256v1", "-genkey", "-noout",
                 "-out", PRIV_PEM)
        os.chmod(PRIV_PEM, 0o600)
    write_public_header()
    return PRIV_PEM


def main(argv):
    if len(argv) >= 1 and argv[0] == "vermagic":
        print("0x%08x" % vermagic())
        return 0
    if len(argv) >= 1 and argv[0] == "genkeys":
        path = generate_keys(force="--force" in argv[1:])
        print("module signing key:", path)
        return 0
    print(f"Usage: {sys.argv[0]} {{vermagic|genkeys [--force]}}", file=sys.stderr)
    return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
