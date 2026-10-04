#!/usr/bin/env python3
"""Write cact_build_time.c with the current build timestamp.

meson re-runs this on every build (`build_always_stale` in
Cact/kernel/core/kern_ver/meson.build), so the boot banner reports the real
build time instead of the last `meson setup`.
"""
import datetime
import sys

with open(sys.argv[1], "w") as out:
    out.write('const char kernel_build_time[] = "%s";\n'
              % datetime.datetime.now().strftime("%Y-%m-%d %H:%M:%S"))
