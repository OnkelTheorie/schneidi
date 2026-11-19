#!/usr/bin/env python3
"""site/img/: the WebP images of the website's feature cards (site/features.json), made from screenshots/*.png.

  tools/site_images.py --write    make site/img/<image>-960.webp and -1920.webp (never upscaled) for every card
                                  whose image is missing or older than screenshots/<image>.png; removes unused files
  tools/site_images.py --force    make all of them again

The website's updater fetches these files at the release tag (at most 1 MB each, checked as WebP) and writes them
into the site's img/ folder; tools/site_figures.py --check checks that they exist and fit. After changing a
screenshot that is on the website, run --write and commit site/img/.

Needs ffmpeg with libwebp, else cwebp. Python 3.8+, standard library only.
"""
import argparse
import json
import os
import shutil
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FEATURES = os.path.join(ROOT, "site", "features.json")
OUT = os.path.join(ROOT, "site", "img")
SHOTS = os.path.join(ROOT, "screenshots")
WIDTHS = (960, 1920)
QUALITY = 82  # same as the website's build.py
MAX_IMAGE = 1_000_000


def png_width(path):
    with open(path, "rb") as f:
        head = f.read(24)
    if head[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError("%s is not a PNG" % path)
    return int.from_bytes(head[16:20], "big")


def encode(src, dst, width):
    fd, tmp = tempfile.mkstemp(suffix=".webp", dir=os.path.dirname(dst))
    os.close(fd)
    try:
        if shutil.which("ffmpeg"):
            cmd = ["ffmpeg", "-v", "error", "-y", "-i", src, "-vf", "scale=%d:-2:flags=lanczos" % width,
                   "-c:v", "libwebp", "-quality", str(QUALITY), "-compression_level", "6", "-f", "webp", tmp]
        elif shutil.which("cwebp"):
            cmd = ["cwebp", "-quiet", "-q", str(QUALITY), "-m", "6", "-resize", str(width), "0", src, "-o", tmp]
        else:
            raise RuntimeError("neither ffmpeg nor cwebp found")
        subprocess.run(cmd, check=True)
        if os.path.getsize(tmp) > MAX_IMAGE:
            raise RuntimeError("%s would be larger than 1 MB; the website would refuse it" % dst)
        os.replace(tmp, dst)
    finally:
        if os.path.exists(tmp):
            os.unlink(tmp)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    g = ap.add_mutually_exclusive_group(required=True)
    g.add_argument("--write", action="store_true", help="make missing or outdated images")
    g.add_argument("--force", action="store_true", help="make all images again")
    a = ap.parse_args()
    with open(FEATURES, encoding="utf-8") as f:
        images = [x["image"] for x in json.load(f)["features"]]
    os.makedirs(OUT, exist_ok=True)
    wanted, made = set(), 0
    for img in images:
        src = os.path.join(SHOTS, img + ".png")
        if not os.path.exists(src):
            print("site_images: screenshots/%s.png is missing" % img, file=sys.stderr)
            return 1
        w = png_width(src)
        for width in WIDTHS:
            if width > w and width != WIDTHS[0]:
                continue  # never upscale; the smallest size is always made
            name = "%s-%d.webp" % (img, width)
            wanted.add(name)
            dst = os.path.join(OUT, name)
            if a.force or not os.path.exists(dst) or os.path.getmtime(dst) < os.path.getmtime(src):
                encode(src, dst, min(width, w))
                made += 1
                print("site_images: wrote site/img/%s" % name)
    for name in sorted(set(os.listdir(OUT)) - wanted):
        os.unlink(os.path.join(OUT, name))
        print("site_images: removed unused site/img/%s" % name)
    print("site_images: %d image(s) made, %d in site/img/" % (made, len(wanted)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
