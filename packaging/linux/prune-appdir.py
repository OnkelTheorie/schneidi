#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-only
# Takes Qt plugins out of an AppDir that linuxdeploy's Qt plugin copied only because they are installed on the build
# machine (KDE's kimageformats - EXR, HEIF, JPEG XL, RAW …, the virtual keyboard), and the libraries that only they
# needed. Libraries the rest of the AppDir needs, and ones nothing links (loaded at run time), stay.
#   prune-appdir.py <AppDir>
import os
import subprocess
import sys

# Image formats schneidi reads through Qt (pictures, icons, titles); everything else (video stills, EXR …) goes
# through its own FFmpeg path
KEEP_IMAGEFORMATS = {"libqgif.so", "libqico.so", "libqjpeg.so", "libqsvg.so"}
DROP = {"platforminputcontexts": {"libqtvirtualkeyboardplugin.so"}}

appdir = sys.argv[1]
lib = os.path.join(appdir, "usr", "lib")
plugins = os.path.join(appdir, "usr", "plugins")


def needed(path):
    out = subprocess.run(["objdump", "-p", path], capture_output=True, text=True).stdout
    return [l.split()[-1] for l in out.splitlines() if l.strip().startswith("NEEDED")]


def elf_files(root):
    for d, _, files in os.walk(root):
        for f in files:
            p = os.path.join(d, f)
            if not os.path.islink(p) and (".so" in f or os.access(p, os.X_OK)):
                with open(p, "rb") as fh:
                    if fh.read(4) == b"\x7fELF":
                        yield p


def closure(starts):
    seen, todo = set(), list(starts)
    while todo:
        p = todo.pop()
        for n in needed(p):
            q = os.path.join(lib, n)
            if n not in seen and os.path.exists(q):
                seen.add(n)
                todo.append(q)
    return seen


roots = list(elf_files(os.path.join(appdir, "usr", "bin"))) + list(elf_files(plugins))
drop = []
for p in roots:
    rel = os.path.relpath(p, plugins)
    kind, name = (rel.split(os.sep, 1) + [""])[:2]
    if kind == "imageformats" and name not in KEEP_IMAGEFORMATS or name in DROP.get(kind, set()):
        drop.append(p)
before = closure(roots)
after = closure([p for p in roots if p not in drop])
gone = sorted(before - after)
size = sum(os.path.getsize(p) for p in drop) + sum(os.path.getsize(os.path.join(lib, n)) for n in gone)
for p in drop:
    os.remove(p)
for n in gone:
    os.remove(os.path.join(lib, n))
print(f">> pruned {len(drop)} plugins and {len(gone)} libraries only they needed ({size / 1e6:.1f} MB)")
for p in drop:
    print("   plugin", os.path.relpath(p, appdir))
for n in gone:
    print("   lib", n)
