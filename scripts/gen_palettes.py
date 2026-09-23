"""One-shot generator for the palette table in lib/gbcore/render/palette.c.

The table has 14 entries, from two sources.

Entries 0 and 1 have a BG ramp chosen here, and their two OBJ ramps are
derived from it so all three share a hue family. Design §2.4 wants blending
between palettes at sprite edges to read as anti-aliasing, and hues that sit
far apart fringe instead.

    0  DMG Green    SameBoy's GB_PALETTE_DMG, Core/display.c at the commit
                    below: the four lit shades, light to dark, leaving out the
                    fifth "LCD off" colour.
    1  Pocket Gray  the fork's original ramp, carried over verbatim.

Entries 2-13 are the Game Boy Color boot ROM's twelve D-pad + button palettes,
the ones a player picks by holding a direction (and A or B) while the CGB
logo shows. They are read from SameBoy's cgb_boot.asm (KeyCombinationPalettes
into PaletteCombinations) and emitted with Nintendo's own OBJ0, OBJ1 and BG
rows, with nothing derived. Several of them use the same ramp for all three
rows, and that is the hardware's choice. Right + B runs dark to light on
purpose: it is the inverted palette.

Source: SameBoy by Lior Halphon, Expat/MIT, which this firmware's
GPL-2.0-or-later accepts. The asm is the same file scripts/gen_cgb_palettes.py
reads, pinned to the same commit:

    curl -sL -o cgb_boot.asm \\
      https://raw.githubusercontent.com/LIJI32/SameBoy/912a17d73d34951979ae0c468afa658a44cf3044/BootROMs/cgb_boot.asm
    python3 scripts/gen_palettes.py cgb_boot.asm

OBJ derivation for entries 0 and 1, per colour:

    chromatic ramp   OBJ0 = saturation x SAT_SCALE
                     OBJ1 = hue rotated by HUE_ROTATE degrees, S and V kept
    achromatic ramp  OBJ0 = value x VAL_DOWN
                     OBJ1 = value x VAL_UP

A ramp counts as achromatic when no colour in it reaches ACHROMATIC_S. The
5/6/5 grid leaves nominally grey values with a percent or two of saturation,
so the threshold applies to the whole ramp, not to each colour. Rotating the
hue of a grey does nothing, which would make OBJ1 a copy of BG, hence the
split.

Run from the project root. It prints the table body to stdout. The output is
committed as literals in palette.c, so this script records how they were made
and can regenerate them. It is not a build step.
"""

import colorsys
import re
import sys

import gen_cgb_palettes

SAT_SCALE = 0.80
HUE_ROTATE = 12.0
VAL_DOWN = 0.88
VAL_UP = 1.14
ACHROMATIC_S = 0.10

# GB_PALETTE_DMG, light to dark, as 8-bit RGB.
DMG_GREEN_RGB = [(0xC6, 0xDE, 0x8C), (0x84, 0xA5, 0x63),
                 (0x39, 0x61, 0x39), (0x08, 0x18, 0x10)]

# The fork's Pocket Gray, verbatim.
POCKET_GRAY_BG = (0xFFFF, 0xB596, 0x6B4D, 0x0000)

# Menu order: each direction, then with A, then with B. The labels are
# SameBoy's comments on KeyCombinationPalettes; the names describe the colours.
KEY_COMBOS = [
    ("Right", "Green"), ("Right + A", "Dark Green"), ("Right + B", "Reverse"),
    ("Left", "Blue"), ("Left + A", "Dark Blue"), ("Left + B", "Grayscale"),
    ("Up", "Brown"), ("Up + A", "Red"), ("Up + B", "Dark Brown"),
    ("Down", "Pastel"), ("Down + A", "Orange"), ("Down + B", "Yellow"),
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


def key_combos(asm):
    """{button label: combination id}, Nintendo's twelve only."""
    block = gen_cgb_palettes.nintendo_only(
        gen_cgb_palettes.between(asm, "KeyCombinationPalettes:", "TrademarkSymbol:"))
    return {label.strip(): int(cid) for cid, label in re.findall(
        r"palette_comb_id\s+(\d+)\s*;\s*\d+,\s*(.+)", block)}


def cgb_entry(combo, colours):
    """OBJ0, OBJ1, BG as RGB565, resolved the way cart/cgb_palette.c does."""
    return tuple(
        tuple(gen_cgb_palettes.to_rgb565(colours[off // 2 + k]) for k in range(4))
        for off in combo)


def entry(idx, name, comment, rows):
    print("    {  /* %2d %s (%s) */" % (idx, name, comment))
    for label, r in zip(("OBJ0", "OBJ1", "BG  "), rows):
        note = ""
        bad = collapsed(r)
        if bad:
            note = "  /* COLLAPSED at %s */" % ",".join(str(i) for i in bad)
        print("        { %s },  /* %s */%s" % (
            ", ".join("0x%04X" % c for c in r), label, note))
    print("    },")


def main(path):
    asm = open(path).read()
    _, _, _, _, combos, colours = gen_cgb_palettes.parse(asm)
    ids = key_combos(asm)
    assert sorted(ids) == sorted(label for label, _ in KEY_COMBOS), ids

    own = [("DMG Green", tuple(to_565(c) for c in DMG_GREEN_RGB)),
           ("Pocket Gray", POCKET_GRAY_BG)]
    for idx, (name, bg) in enumerate(own):
        obj0, obj1, achromatic = derive(bg)
        entry(idx, name, "achromatic" if achromatic else "chromatic",
              (obj0, obj1, bg))

    for idx, (label, name) in enumerate(KEY_COMBOS, start=len(own)):
        entry(idx, name, "CGB %s, combination %d" % (label, ids[label]),
              cgb_entry(combos[ids[label]], colours))


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else "cgb_boot.asm")
