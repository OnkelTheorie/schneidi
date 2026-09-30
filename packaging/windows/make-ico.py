#!/usr/bin/env python3
"""assets/schneidi.ico aus assets/schneidi.svg erzeugen (Programm-, Installer- und Startmenü-Symbol unter Windows).

Nur nach einer Änderung am Logo nötig, die .ico liegt im Repo. Braucht rsvg-convert (MSYS2: mingw-w64-ucrt-x86_64-librsvg).
Aufruf aus dem Projektordner:  python3 packaging/windows/make-ico.py
ICO mit PNG-Einträgen (ab Windows Vista üblich).
"""
import os, struct, subprocess

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SVG = os.path.join(ROOT, 'assets', 'schneidi.svg')
ICO = os.path.join(ROOT, 'assets', 'schneidi.ico')
SIZES = [16, 20, 24, 32, 40, 48, 64, 128, 256]

images = [subprocess.run(['rsvg-convert', '-w', str(s), '-h', str(s), SVG], check=True, capture_output=True).stdout
          for s in SIZES]
out = struct.pack('<HHH', 0, 1, len(images))
offset = 6 + 16 * len(images)
for s, png in zip(SIZES, images):
    out += struct.pack('<BBBBHHII', s % 256, s % 256, 0, 0, 1, 32, len(png), offset)
    offset += len(png)
with open(ICO, 'wb') as f:
    f.write(out + b''.join(images))
print(ICO, offset, 'Bytes')
