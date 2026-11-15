#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-only
# Checks that no ELF file in an AppDir needs newer glibc/libstdc++ symbol versions than the oldest system the AppImage
# is for (default Ubuntu 22.04: GLIBC 2.35, GLIBCXX 3.4.30, CXXABI 1.3.13). Prints the highest version of each and
# every file above a limit; exit code 1 if there is one.
#   check-abi.py <AppDir> [--glibc 2.35] [--glibcxx 3.4.30] [--cxxabi 1.3.13]
import argparse
import os
import re
import subprocess
import sys

ap = argparse.ArgumentParser()
ap.add_argument("appdir")
ap.add_argument("--glibc", default="2.35")
ap.add_argument("--glibcxx", default="3.4.30")
ap.add_argument("--cxxabi", default="1.3.13")
args = ap.parse_args()
limits = {"GLIBC": args.glibc, "GLIBCXX": args.glibcxx, "CXXABI": args.cxxabi}


def ver(s):
    return tuple(int(x) for x in s.split("."))


def is_elf(path):
    try:
        with open(path, "rb") as f:
            return f.read(4) == b"\x7fELF"
    except OSError:
        return False


pattern = re.compile(r"\b(GLIBC|GLIBCXX|CXXABI)_([0-9]+(?:\.[0-9]+)+)\b")
highest = {k: ("0", "") for k in limits}
over = []
count = 0
for d, _, files in os.walk(args.appdir):
    for f in files:
        p = os.path.join(d, f)
        if os.path.islink(p) or not is_elf(p):
            continue
        count += 1
        # Only the versions the file needs (undefined symbols / verneed), not the ones a library defines itself
        out = subprocess.run(["objdump", "-p", p], capture_output=True, text=True).stdout
        need = out.split("Version References:", 1)[1] if "Version References:" in out else ""
        worst = {}
        for name, v in pattern.findall(need):
            if ver(v) > ver(worst.get(name, "0")):
                worst[name] = v
        for name, v in worst.items():
            if ver(v) > ver(highest[name][0]):
                highest[name] = (v, os.path.relpath(p, args.appdir))
            if ver(v) > ver(limits[name]):
                over.append(f"{os.path.relpath(p, args.appdir)}: {name}_{v}")

print(f"{count} ELF files")
for name, (v, where) in highest.items():
    print(f"highest {name}_{v} (limit {limits[name]}){'  ' + where if where else ''}")
if over:
    print("!! above the limit:", file=sys.stderr)
    for line in over:
        print("   " + line, file=sys.stderr)
    sys.exit(1)
