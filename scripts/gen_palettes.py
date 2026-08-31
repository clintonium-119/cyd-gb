"""One-shot generator for the OBJ sub-ramps in lib/gbcore/render/palette.c.

Each palette's BG ramp is the fork's original four colours, kept verbatim. The
two OBJ ramps are derived from it so all three share a hue family: design §2.4
wants cross-palette blending at sprite edges to read as anti-aliasing, and
far-apart hues fringe instead.

Derivation, per colour:

    chromatic ramp   OBJ0 = saturation x SAT_SCALE
                     OBJ1 = hue rotated by HUE_ROTATE degrees, S and V kept
    achromatic ramp  OBJ0 = value x VAL_DOWN
                     OBJ1 = value x VAL_UP

A ramp counts as achromatic when no colour in it reaches ACHROMATIC_S; the
5/6/5 grid leaves nominally grey values with a percent or two of saturation, so
the threshold is on the ramp, not on the individual colour. Hue rotation on a
grey is a no-op, which would make OBJ1 a duplicate of BG, hence the split.

Run from the project root; it prints the table body to stdout. The output is
committed as literals in palette.c — this script is a record of how they were
derived and a way to regenerate them, not a build step.
"""

import colorsys

SAT_SCALE = 0.80
HUE_ROTATE = 12.0
VAL_DOWN = 0.88
VAL_UP = 1.14
ACHROMATIC_S = 0.10

# The fork's 20 ramps, verbatim from src/emulator_bridge.cpp as of WS-03.
BG = [
    (0x9FE5, 0x4F64, 0x2542, 0x0261),
    (0xFFFF, 0xAD55, 0x52AA, 0x0000),
    (0xFFFF, 0xB596, 0x6B4D, 0x0000),
    (0xFFDF, 0xD68F, 0x7A4B, 0x1082),
    (0xBF5F, 0x6CDF, 0x339F, 0x0019),
    (0xFFF0, 0xFC00, 0x8800, 0x2000),
    (0xE71C, 0x9CD3, 0x4228, 0x0000),
    (0xFFFF, 0xFE20, 0xC800, 0x4000),
    (0xAFFF, 0x5F5F, 0x2D1F, 0x0019),
    (0xFFF0, 0xBDE0, 0x5AE0, 0x0120),
    (0xFFFF, 0xFD20, 0xAB00, 0x4000),
    (0xFFDF, 0xF71C, 0xAA13, 0x3808),
    (0xCFFF, 0x867F, 0x433F, 0x0019),
    (0xFFB6, 0xD52A, 0x8A08, 0x3000),
    (0xFFFF, 0xBF5F, 0x5F1F, 0x0019),
    (0xFFF8, 0xFCC0, 0xC880, 0x6000),
    (0xEF3C, 0x867F, 0x4179, 0x0000),
    (0xFFFF, 0x07FF, 0x001F, 0x0000),
    (0x0000, 0x4228, 0xAD55, 0xFFFF),
    (0xFE60, 0xAB00, 0x5000, 0x0000),
]

NAMES = [
    "Classic Green", "Original DMG", "Pocket Gray", "Warm Sepia", "Cool Blue",
    "Autumn", "Grayscale", "Lava", "Ocean", "Forest",
    "Sunset", "Cherry", "Ice", "Chocolate", "Mint",
    "Peach", "Lavender", "Neon", "Inverted", "Gold",
]


def to_rgb(c565):
    r = (c565 >> 11) & 0x1F
    g = (c565 >> 5) & 0x3F
    b = c565 & 0x1F
    return (
        (r * 255 + 15) // 31,
        (g * 255 + 31) // 63,
        (b * 255 + 15) // 31,
    )


def to_565(rgb):
    r, g, b = (max(0, min(255, int(round(v)))) for v in rgb)
    return (((r * 31 + 127) // 255) << 11) | \
           (((g * 63 + 127) // 255) << 5) | \
           ((b * 31 + 127) // 255)


def saturation(c565):
    r, g, b = to_rgb(c565)
    return colorsys.rgb_to_hsv(r / 255.0, g / 255.0, b / 255.0)[1]


def shift(c565, sat_scale=1.0, hue_deg=0.0, val_scale=1.0):
    r, g, b = to_rgb(c565)
    h, s, v = colorsys.rgb_to_hsv(r / 255.0, g / 255.0, b / 255.0)
    h = (h + hue_deg / 360.0) % 1.0
    s = min(1.0, s * sat_scale)
    v = min(1.0, v * val_scale)
    return to_565(tuple(x * 255.0 for x in colorsys.hsv_to_rgb(h, s, v)))


def derive(ramp):
    achromatic = max(saturation(c) for c in ramp) < ACHROMATIC_S
    if achromatic:
        obj0 = tuple(shift(c, val_scale=VAL_DOWN) for c in ramp)
        obj1 = tuple(shift(c, val_scale=VAL_UP) for c in ramp)
    else:
        obj0 = tuple(shift(c, sat_scale=SAT_SCALE) for c in ramp)
        obj1 = tuple(shift(c, hue_deg=HUE_ROTATE) for c in ramp)
    return obj0, obj1, achromatic


def collapsed(ramp):
    """Adjacent entries that became equal — the ramp lost a shade."""
    return [i for i in range(3) if ramp[i] == ramp[i + 1]]


def main():
    for idx, ramp in enumerate(BG):
        obj0, obj1, achromatic = derive(ramp)
        kind = "achromatic" if achromatic else "chromatic"
        print("    {  /* %2d %s (%s, max S %.3f) */" % (
            idx, NAMES[idx], kind, max(saturation(c) for c in ramp)))
        for label, r in (("OBJ0", obj0), ("OBJ1", obj1), ("BG  ", ramp)):
            note = ""
            bad = collapsed(r)
            if bad:
                note = "  /* COLLAPSED at %s */" % ",".join(str(i) for i in bad)
            print("        { %s },  /* %s */%s" % (
                ", ".join("0x%04X" % c for c in r), label, note))
        print("    },")


if __name__ == "__main__":
    main()
