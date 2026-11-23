#!/usr/bin/env python3
# Builds assets/fonts/schneidi-symbols-*.ttf: the symbol glyphs the UI writes as text (▦ ✦ ☰ ⏮ ⤢ …), cut out of
# DejaVu Sans, Noto Sans Symbols 2 and Noto Sans Math (Debian: fonts-dejavu-core, fonts-noto-core). Platforms whose
# fonts lack them (Android drew boxes) load these as fallbacks (app/Theme.cpp). Needs fontTools:
#   python3 tools/make_symbol_fonts.py
# The families are renamed ("schneidi Symbols …") so that they never stand in for the system's own DejaVu/Noto.
import sys
from pathlib import Path
from fontTools import subset
from fontTools.ttLib import TTFont

# Unicode blocks of UI symbols: arrows, math operators, technical (⏮ ⏸), geometric shapes, misc symbols, dingbats,
# supplemental arrows (⤢), misc math symbols B (⧓), misc symbols and arrows
BLOCKS = [(0x2190, 0x21FF), (0x2200, 0x22FF), (0x2300, 0x23FF), (0x25A0, 0x25FF), (0x2600, 0x26FF), (0x2700, 0x27BF),
          (0x27F0, 0x27FF), (0x2900, 0x297F), (0x2980, 0x29FF), (0x2B00, 0x2BFF), (0x00B6, 0x00B6)]
SOURCES = [  # later fonts only add what the earlier ones lack
    ("/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", "schneidi Symbols A"),
    ("/usr/share/fonts/truetype/noto/NotoSansSymbols2-Regular.ttf", "schneidi Symbols B"),
    ("/usr/share/fonts/truetype/noto/NotoSansMath-Regular.ttf", "schneidi Symbols C"),
]

def main():
    out = Path(__file__).resolve().parent.parent / "assets" / "fonts"
    out.mkdir(parents=True, exist_ok=True)
    wanted = {c for a, b in BLOCKS for c in range(a, b + 1)}
    done = set()
    for path, family in SOURCES:
        font = TTFont(path)
        have = set(font.getBestCmap()) & wanted - done
        if not have:
            continue
        opts = subset.Options()
        opts.layout_features = ["*"]
        opts.name_IDs = ["*"]
        opts.notdef_outline = True
        sub = subset.Subsetter(opts)
        sub.populate(unicodes=sorted(have))
        sub.subset(font)
        for rec in font["name"].names:
            if rec.nameID in (1, 16):
                rec.string = family
            elif rec.nameID in (4,):
                rec.string = family + " Regular"
            elif rec.nameID == 6:
                rec.string = family.replace(" ", "") + "-Regular"
        name = out / ("schneidi-symbols-" + family.split()[-1].lower() + ".ttf")
        font.save(name)
        done |= have
        print(f"{name.name}: {len(have)} glyphs, {name.stat().st_size} bytes")
    return 0

if __name__ == "__main__":
    sys.exit(main())
