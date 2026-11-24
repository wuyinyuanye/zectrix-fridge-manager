#!/usr/bin/env python3
"""Generate ZPX1 12x12 dot-matrix Chinese + ASCII bitmap font from Zpix (zpix.ttf).
Format (ZPX1):
  Header (12 bytes): 'ZPX1', uint16 glyph_count, uint16 width(12), uint16 height(12), uint16 reserved
  Entries (glyph_count * 4 bytes): sorted uint16 codepoint, uint8 advance, uint8 reserved
  Bitmaps (glyph_count * 24 bytes): 12 rows x uint16 big-endian (bits 15..4 = cols 0..11, 1=ink)
"""

import argparse
import struct
from pathlib import Path
from PIL import Image, ImageDraw, ImageFont

EXTRA_CHARS = (
    "°℃↑↓←→✓✗★☆●○■□◆◇▲▼·…—～！？，。：；、（）【】「」《》“”‘’￥＋－×÷"
    "猕柚莓蚝焗焖焯腌涮卤煲鲜冻藏购期剩天分秒时钟菜谱蛋奶肉鱼虾蟹果蔬"
    "葱姜蒜椒酱醋糖盐油米面豆奶酪芝士牛羊猪鸡鸭鹅虾蟹贝螺芹菠藕茄瓜"
)


def collect_codepoints() -> list[int]:
    cps: set[int] = set(range(0x20, 0x7F))
    for ch in EXTRA_CHARS:
        if ord(ch) <= 0xFFFF:
            cps.add(ord(ch))
    for zone in range(0xB0, 0xF8):
        for pos in range(0xA1, 0xFF):
            try:
                ch = bytes([zone, pos]).decode("gb2312")
                if len(ch) == 1 and ord(ch) <= 0xFFFF:
                    cps.add(ord(ch))
            except UnicodeDecodeError:
                pass
    return sorted(cps)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--font", required=True)
    parser.add_argument("--out", required=True)
    args = parser.parse_args()

    font = ImageFont.truetype(args.font, size=12)
    cps = collect_codepoints()
    entries = bytearray()
    bitmaps = bytearray()

    for cp in cps:
        ch = chr(cp)
        is_ascii = cp < 0x80
        adv = max(4, min(12, int(round(font.getlength(ch)))))
        if not is_ascii:
            adv = 12

        img = Image.new("1", (12, 12), 0)
        d = ImageDraw.Draw(img)
        d.fontmode = "1"
        if ch != " ":
            d.text((0, 0), ch, fill=1, font=font)

        bmp = bytearray(24)
        px = img.load()
        for row in range(12):
            word = 0
            for col in range(12):
                if px[col, row]:
                    word |= 1 << (15 - col)
            bmp[row * 2] = (word >> 8) & 0xFF
            bmp[row * 2 + 1] = word & 0xFF

        entries += struct.pack("<HBB", cp, adv, 0)
        bitmaps += bmp

    header = struct.pack("<4sHHH2x", b"ZPX1", len(cps), 12, 12)
    out = Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_bytes(header + entries + bitmaps)
    print(f"Generated ZPX1: {len(cps)} glyphs, {out.stat().st_size} bytes -> {out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
