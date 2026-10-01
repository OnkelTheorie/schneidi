#!/usr/bin/env python3
"""Mitgelieferte Looks (Kategorie „schneidi“ in der Effects Library) als 3D-LUTs (.cube) erzeugen.

  tools/gen_luts.py   schreibt assets/luts/*.cube neu (Größe 17, Eingang/Ausgang Rec. 709 gamma-kodiert)

Die Anzeigenamen stehen im Code (core/EffectFolders.cpp), hier nur die Dateinamen (= IDs, nie umbenennen:
Projektdateien verweisen per Pfad ":/luts/<id>.cube" darauf).
"""
import math, os

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, 'assets', 'luts')
SIZE = 17


def clamp(x):
    return 0.0 if x < 0 else 1.0 if x > 1 else x


def luma(r, g, b):
    return 0.2126 * r + 0.7152 * g + 0.0722 * b


def sat(rgb, s):
    y = luma(*rgb)
    return [y + (c - y) * s for c in rgb]


def scurve(x, k):
    """Weiche S-Kurve um 0,5 (k > 0 = mehr Kontrast), 0 und 1 bleiben."""
    x = clamp(x)
    return x + k * x * (1 - x) * (2 * x - 1)  # k < 1: bleibt monoton


def contrast(x, k, pivot=0.45):
    return clamp((x - pivot) * k + pivot)


def lift_gain(x, lift, gain):
    return clamp(lift + x * (gain - lift))


def mix(a, b, t):
    return a + (b - a) * t


def smooth(e0, e1, x):
    t = clamp((x - e0) / (e1 - e0))
    return t * t * (3 - 2 * t)


def tint(rgb, shadows, highlights, amount=1.0):
    """Schatten/Lichter in eine Farbe schieben (Split Toning), Helligkeit bleibt etwa gleich."""
    y = luma(*rgb)
    ws, wh = (1 - smooth(0.0, 0.6, y)) * amount, smooth(0.4, 1.0, y) * amount
    out = [c + ws * s + wh * h for c, s, h in zip(rgb, shadows, highlights)]
    d = y - luma(*out)
    return [c + d for c in out]


# ---- Looks: Funktion (r, g, b) -> (r, g, b), alles 0..1 ----

def teal_orange(r, g, b):
    rgb = tint([r, g, b], (-0.06, 0.02, 0.07), (0.07, 0.01, -0.07))
    rgb = sat(rgb, 1.1)
    return [scurve(c, 0.35) for c in rgb]


def warm(r, g, b):
    return [clamp(r * 1.06 + 0.01), clamp(g * 1.01 + 0.005), clamp(b * 0.9)]


def kuehl(r, g, b):
    return [clamp(r * 0.92), clamp(g * 0.99 + 0.005), clamp(b * 1.07 + 0.01)]


def verblasst(r, g, b):
    rgb = sat([r, g, b], 0.75)
    return [lift_gain(c, 0.08, 0.92) for c in rgb]


def schwarzweiss(r, g, b):
    y = scurve(luma(r, g, b), 0.15)
    return [y, y, y]


def sw_kontrast(r, g, b):
    y = 0.5 * r + 0.4 * g + 0.1 * b  # wie ein Rotfilter: blauer Himmel dunkler
    y = scurve(contrast(y, 1.25), 0.4)
    return [y, y, y]


def sepia(r, g, b):
    y = scurve(luma(r, g, b), 0.1)
    return [clamp(y * 1.07 + 0.03), clamp(y * 0.95 + 0.02), clamp(y * 0.78)]


def bleach_bypass(r, g, b):
    y = luma(r, g, b)
    out = []
    for c in (r, g, b):
        ov = 2 * c * y if y < 0.5 else 1 - 2 * (1 - c) * (1 - y)  # Überlagern mit der Helligkeit
        out.append(mix(c, ov, 0.7))
    return [scurve(c, 0.2) for c in sat(out, 0.55)]


def retro(r, g, b):
    rgb = sat([r, g, b], 0.7)
    rgb = tint(rgb, (-0.02, 0.03, 0.01), (0.06, 0.03, -0.06))
    return [lift_gain(c, 0.06, 0.94) for c in rgb]


def nacht(r, g, b):
    rgb = sat([r, g, b], 0.35)
    rgb = [rgb[0] * 0.55, rgb[1] * 0.65, rgb[2] * 0.85 + 0.03]
    return [clamp(scurve(c, 0.2) ** 1.15) for c in rgb]


def kino(r, g, b):
    rgb = tint([r, g, b], (-0.03, 0.0, 0.03), (0.03, 0.01, -0.02))
    rgb = sat(rgb, 1.08)
    return [scurve(contrast(c, 1.08), 0.3) for c in rgb]


def lebendig(r, g, b):
    return [scurve(c, 0.15) for c in sat([r, g, b], 1.35)]


def cross_process(r, g, b):
    return [scurve(contrast(r, 1.15), 0.3), scurve(g, 0.25), lift_gain(b, 0.12, 0.82)]


def pastell(r, g, b):
    rgb = sat([r, g, b], 0.6)
    return [clamp(0.12 + c * 0.86 + 0.04 * math.sin(math.pi * c)) for c in rgb]


LOOKS = {
    'teal-orange': teal_orange, 'warm': warm, 'kuehl': kuehl, 'verblasst': verblasst,
    'schwarzweiss': schwarzweiss, 'schwarzweiss-kontrast': sw_kontrast, 'sepia': sepia,
    'bleach-bypass': bleach_bypass, 'retro': retro, 'nacht': nacht, 'kino': kino, 'lebendig': lebendig,
    'cross-process': cross_process, 'pastell': pastell,
}


def main():
    os.makedirs(OUT, exist_ok=True)
    for name, fn in LOOKS.items():
        lines = [f'# schneidi: {name} (tools/gen_luts.py)', f'TITLE "{name}"', f'LUT_3D_SIZE {SIZE}']
        n = SIZE - 1
        for bi in range(SIZE):  # .cube: R läuft am schnellsten
            for gi in range(SIZE):
                for ri in range(SIZE):
                    o = [clamp(v) for v in fn(ri / n, gi / n, bi / n)]
                    lines.append(' '.join(f'{v:.4f}' for v in o))
        with open(os.path.join(OUT, name + '.cube'), 'w', newline='\n') as f:
            f.write('\n'.join(lines) + '\n')
    print(f'{len(LOOKS)} LUTs in {OUT}')


if __name__ == '__main__':
    main()
