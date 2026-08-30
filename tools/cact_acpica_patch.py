#!/usr/bin/env python3
"""Idempotent ACPICA freestanding patch for the Cact kernel.

ACPICA auto-detects Linux (aclinux.h) and, in its user-space (!__KERNEL__)
mode, defines ACPI_USE_STANDARD_HEADERS, which pulls host <stdlib.h>,
<string.h>, <ctype.h>.  Those conflict with the kernel's freestanding klib.h
declarations (atoi/strncpy/strncmp/strlen/strlcpy/strlcat).

For a freestanding kernel we build ACPICA with -DCACT_ACPI_FREESTANDING and
gate the system-header includes on that macro, so ACPICA uses its own minimal
string/memory helpers instead of host libc headers.

Usage: python3 tools/cact_acpica_patch.py <ACPICA_DIR>
"""

import os
import sys

BUILD_MARK = "CACT_ACPI_FREESTANDING"

OLD_IFDEF = "#ifdef ACPI_USE_STANDARD_HEADERS"
NEW_IFDEF = "#if defined(ACPI_USE_STANDARD_HEADERS) && !defined(CACT_ACPI_FREESTANDING)"


def patch_file(path: str) -> bool:
    if not os.path.isfile(path):
        return False
    with open(path, "r", encoding="utf-8") as f:
        text = f.read()
    if BUILD_MARK in text:
        return False
    if OLD_IFDEF not in text:
        return False
    text = text.replace(OLD_IFDEF, NEW_IFDEF)
    with open(path, "w", encoding="utf-8") as f:
        f.write(text)
    return True


def main() -> int:
    if len(sys.argv) != 2:
        print(f"Usage: {sys.argv[0]} <ACPICA_DIR>", file=sys.stderr)
        return 1
    acpica = sys.argv[1]

    # acenv.h: gate the <stdlib.h>/<string.h>/<ctype.h> block.
    acenv = os.path.join(acpica, "source", "include", "platform", "acenv.h")
    # aclinux.h: gate the <stddef.h>/<unistd.h>/<stdint.h>/offsetof block.
    aclinux = os.path.join(acpica, "source", "include", "platform", "aclinux.h")

    changed = False
    if patch_file(acenv):
        changed = True
        print(f"[cact_acpica] patched {acenv}")
    if patch_file(aclinux):
        changed = True
        print(f"[cact_acpica] patched {aclinux}")
    if not changed:
        print("[cact_acpica] already patched (or targets missing) — nothing to do")
    return 0


if __name__ == "__main__":
    sys.exit(main())
