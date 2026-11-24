#!/usr/bin/env python3
"""Extract the two native 1bpp Chinese LVGL bitmap fonts (17px body + 29px title)
from the user's personal ZECTRIX NOTE4 3.7.5 firmware backup into compact ZFN2 format:

Header (12 bytes):
  magic[4]      = 'ZFN2'
  glyph_count   = uint16_t
  line_height   = uint8_t
  base_line     = uint8_t
  bitmap_bytes  = uint32_t

Entries (glyph_count * 12 bytes, sorted by codepoint):
  uint16_t codepoint
  uint8_t  adv_w
  uint8_t  box_w
  uint8_t  box_h
  int8_t   ofs_x
  int8_t   ofs_y
  uint8_t  reserved
  uint32_t bmp_offset

Bitmap pool (bitmap_bytes): tightly packed MSB-first 1bpp bits per glyph.
"""

import argparse
import struct
from pathlib import Path

DROM_FILE_OFF = 0x020020
DROM_VADDR = 0x3C290020
DROM_LEN = 0x1F6638


def extract_font(drom: bytes, dsc_vaddr: int, line_h: int, base_line: int, out_path: Path) -> None:
    def v2f(v: int) -> int:
        return v - DROM_VADDR

    off = v2f(dsc_vaddr)
    bmp_v, gdsc_v, cmaps_v, _ = struct.unpack("<IIII", drom[off:off + 16])
    _, bits = struct.unpack("<HH", drom[off + 16:off + 20])
    cmap_num = bits & 0x1FF

    cp_to_gid: dict[int, int] = {}
    co = v2f(cmaps_v)
    for _ in range(cmap_num):
        rstart, rlen, gid, ulist, ofslist, llen, ctype = struct.unpack("<IHHIIIB", drom[co:co + 21])
        co += 24
        if ctype == 2:  # FORMAT0_TINY
            for k in range(rlen):
                cp = rstart + k
                if cp <= 0xFFFF:
                    cp_to_gid[cp] = gid + k
        elif ctype == 0:  # FORMAT0_FULL
            for k in range(rlen):
                cp = rstart + k
                if cp > 0xFFFF:
                    continue
                if ofslist:
                    d = drom[v2f(ofslist) + k]
                    if k == 0 or d != 0:
                        cp_to_gid[cp] = gid + d
                else:
                    cp_to_gid[cp] = gid + k
        elif ctype == 3:  # SPARSE_TINY
            uo = v2f(ulist)
            for k in range(llen):
                ofs = struct.unpack("<H", drom[uo + k * 2:uo + k * 2 + 2])[0]
                cp = rstart + ofs
                if cp <= 0xFFFF:
                    cp_to_gid[cp] = gid + k
        elif ctype == 1:  # SPARSE_FULL
            uo = v2f(ulist)
            oo = v2f(ofslist)
            for k in range(llen):
                ofs = struct.unpack("<H", drom[uo + k * 2:uo + k * 2 + 2])[0]
                gofs = struct.unpack("<H", drom[oo + k * 2:oo + k * 2 + 2])[0]
                cp = rstart + ofs
                if cp <= 0xFFFF:
                    cp_to_gid[cp] = gid + gofs

    go = v2f(gdsc_v)
    bo = v2f(bmp_v)

    entries = bytearray()
    bitmaps = bytearray()
    sorted_cps = sorted(cp_to_gid.keys())

    for cp in sorted_cps:
        gid = cp_to_gid[cp]
        bmp_idx, adv_fp, box_w, box_h, ofs_x, ofs_y = struct.unpack(
            "<IIHHhh", drom[go + gid * 16:go + gid * 16 + 16]
        )
        adv_w = max(1, min(255, int(round(adv_fp / 16.0))))
        bw = min(255, box_w)
        bh = min(255, box_h)
        ox = max(-128, min(127, ofs_x))
        oy = max(-128, min(127, ofs_y))
        nbytes = (bw * bh + 7) // 8
        bmp_off = len(bitmaps)
        if nbytes > 0:
            bitmaps += drom[bo + bmp_idx:bo + bmp_idx + nbytes]
        entries += struct.pack("<HBBBbbBI", cp, adv_w, bw, bh, ox, oy, 0, bmp_off)

    header = struct.pack("<4sHBBI", b"ZFN2", len(sorted_cps), line_h, base_line, len(bitmaps))
    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_bytes(header + entries + bitmaps)
    print(f"Extracted {out_path.name}: {len(sorted_cps)} glyphs, lh={line_h}, {out_path.stat().st_size} bytes")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--firmware", required=True)
    ap.add_argument("--out-dir", required=True)
    args = ap.parse_args()

    data = Path(args.firmware).read_bytes()
    drom = data[DROM_FILE_OFF:DROM_FILE_OFF + DROM_LEN]
    out_dir = Path(args.out_dir)

    extract_font(drom, 0x3C3B22E0, 17, 3, out_dir / "official_font_17.bin")
    extract_font(drom, 0x3C31F8A4, 29, 6, out_dir / "official_font_29.bin")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
