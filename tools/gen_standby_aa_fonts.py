#!/usr/bin/env python3
"""Generate nibble-packed 4bpp (0..15) anti-aliased numeral glyphs -> main/standby_aa_fonts.h.

Packed 2 pixels per byte (high nibble = even pixel, low nibble = odd pixel) so all 5
high-craft typography sizes together use only ~13 KB of flash ROM.
"""
import os
from PIL import Image, ImageDraw, ImageFont

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "main", "standby_aa_fonts.h")

FONTS = [
    ("kFontHelvThin68", "/System/Library/Fonts/HelveticaNeue.ttc", 12, 68, "0123456789:./-"),
    ("kFontHelvLight82", "/System/Library/Fonts/HelveticaNeue.ttc", 12, 82, "0123456789:./-"),
    ("kFontHelvLight30", "/System/Library/Fonts/HelveticaNeue.ttc", 7, 30, "0123456789%:/.-"),
    ("kFontHelvMed18", "/System/Library/Fonts/HelveticaNeue.ttc", 7, 19, "0123456789%"),
    ("kFontNewYork82", "/System/Library/Fonts/NewYork.ttf", 0, 82, "0123456789-"),
]

lines = [
    "#ifndef ZECTRIX_STANDBY_AA_FONTS_H_",
    "#define ZECTRIX_STANDBY_AA_FONTS_H_",
    "",
    "#include <cstdint>",
    "",
    "namespace standby_aa {",
    "",
    "struct Glyph {",
    "    char ch;",
    "    uint8_t w, h;",
    "    int8_t ox, oy;",
    "    uint8_t adv;",
    "    uint32_t byte_offset;  // nibble-packed 4bpp alpha (2 px/byte, high nibble first)",
    "};",
    "",
    "struct Font {",
    "    const Glyph* glyphs;",
    "    int count;",
    "    const uint8_t* data;",
    "};",
    "",
]

total_bytes = 0
for name, path, idx, size, chars in FONTS:
    font = ImageFont.truetype(path, size, index=idx)
    packed = []
    glyph_defs = []
    bbox0 = font.getbbox("0", anchor="ls")
    base_top = bbox0[1]

    for ch in chars:
        adv = int(round(font.getlength(ch)))
        W, H = size * 2, size * 2
        img = Image.new("L", (W, H), 0)
        d = ImageDraw.Draw(img)
        baseline_y = size + 16
        d.text((20, baseline_y), ch, fill=255, font=font, anchor="ls")
        bb = img.getbbox()
        if bb is None:
            glyph_defs.append((ch, 0, 0, 0, 0, adv, len(packed)))
            continue
        x0, y0, x1, y1 = bb
        gw, gh = x1 - x0, y1 - y0
        ox = x0 - 20
        oy = y0 - (baseline_y + base_top)
        off = len(packed)
        px = img.load()
        nibbles = []
        for yy in range(y0, y1):
            for xx in range(x0, x1):
                nibbles.append(min(15, (px[xx, yy] + 8) // 17))
        if len(nibbles) & 1:
            nibbles.append(0)
        for i in range(0, len(nibbles), 2):
            packed.append((nibbles[i] << 4) | nibbles[i + 1])
        glyph_defs.append((ch, gw, gh, ox, oy, adv, off))

    total_bytes += len(packed)
    lines.append(f"static const uint8_t {name}_data[] = {{")
    for i in range(0, len(packed), 20):
        chunk = ",".join(f"0x{v:02x}" for v in packed[i:i + 20])
        lines.append(f"    {chunk},")
    lines.append("};")
    lines.append(f"static const Glyph {name}_glyphs[] = {{")
    for ch, gw, gh, ox, oy, adv, off in glyph_defs:
        lines.append(f"    {{'{ch}', {gw}, {gh}, {ox}, {oy}, {adv}, {off}}},")
    lines.append("};")
    lines.append(f"static const Font {name} = {{{name}_glyphs, {len(glyph_defs)}, {name}_data}};")
    lines.append("")

lines += [
    "}  // namespace standby_aa",
    "",
    "#endif  // ZECTRIX_STANDBY_AA_FONTS_H_",
    "",
]

with open(OUT, "w") as f:
    f.write("\n".join(lines))
print(f"Wrote {OUT}: {total_bytes} packed ROM bytes ({total_bytes / 1024:.1f} KB)")
