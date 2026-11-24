#include "pixel_canvas.h"

#include <algorithm>
#include <cstring>

#include "qrcodegen.h"

extern "C" {
extern const uint8_t kOfficialFont17Start[] asm("_binary_official_font_17_bin_start");
extern const uint8_t kOfficialFont17End[] asm("_binary_official_font_17_bin_end");
extern const uint8_t kOfficialFont29Start[] asm("_binary_official_font_29_bin_start");
extern const uint8_t kOfficialFont29End[] asm("_binary_official_font_29_bin_end");
}

namespace {

uint16_t ReadLe16(const uint8_t* p) {
    return static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8);
}

uint32_t ReadLe32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) |
           (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

}  // namespace

void PixelCanvas::LoadFont(const uint8_t* start, const uint8_t* end, LoadedFont* out) {
    const size_t total = static_cast<size_t>(end - start);
    if (total < 12 || std::memcmp(start, "ZFN2", 4) != 0) return;
    const uint16_t count = ReadLe16(start + 4);
    const uint8_t lh = start[6];
    const uint8_t bl = start[7];
    const uint32_t bmp_bytes = ReadLe32(start + 8);
    const size_t entries_bytes = static_cast<size_t>(count) * sizeof(FontEntry);
    if (12 + entries_bytes + bmp_bytes > total) return;
    out->glyph_count = count;
    out->line_height = lh;
    out->base_line = bl;
    out->bitmap_bytes = bmp_bytes;
    out->entries = reinterpret_cast<const FontEntry*>(start + 12);
    out->bitmaps = start + 12 + entries_bytes;
}

PixelCanvas::PixelCanvas() {
    buf4_.assign(ZECTRIX_EPD_4BPP_FRAME_BYTES, 0xFF);
    buf1_.fill(0xFF);
    LoadFont(kOfficialFont17Start, kOfficialFont17End, &font17_);
    LoadFont(kOfficialFont29Start, kOfficialFont29End, &font29_);
}

void PixelCanvas::SetMode4bpp(bool enable_4bpp) {
    mode_4bpp_ = enable_4bpp;
}

void PixelCanvas::Clear(uint8_t gray) {
    gray &= 0x0F;
    if (mode_4bpp_) {
        const uint8_t packed = static_cast<uint8_t>((gray << 4) | gray);
        std::fill(buf4_.begin(), buf4_.end(), packed);
    } else {
        buf1_.fill(gray >= 8 ? 0xFF : 0x00);
    }
}

void PixelCanvas::Pixel(int x, int y, uint8_t gray) {
    if (x < 0 || x >= kWidth || y < 0 || y >= kHeight) return;
    gray &= 0x0F;
    if (mode_4bpp_) {
        const size_t idx = static_cast<size_t>(y) * kStride4bpp + (x >> 1);
        if ((x & 1) == 0) {
            buf4_[idx] = static_cast<uint8_t>((buf4_[idx] & 0x0F) | (gray << 4));
        } else {
            buf4_[idx] = static_cast<uint8_t>((buf4_[idx] & 0xF0) | gray);
        }
    } else {
        const size_t idx = static_cast<size_t>(y) * kStride1bpp + (x >> 3);
        const uint8_t mask = static_cast<uint8_t>(0x80u >> (x & 7));
        if (gray < 8) {
            buf1_[idx] &= static_cast<uint8_t>(~mask);
        } else {
            buf1_[idx] |= mask;
        }
    }
}

void PixelCanvas::FillRect(int x, int y, int w, int h, uint8_t gray) {
    const int x0 = std::max(0, x);
    const int y0 = std::max(0, y);
    const int x1 = std::min(kWidth, x + w);
    const int y1 = std::min(kHeight, y + h);
    for (int py = y0; py < y1; ++py) {
        for (int px = x0; px < x1; ++px) {
            Pixel(px, py, gray);
        }
    }
}

void PixelCanvas::Rect(int x, int y, int w, int h, uint8_t gray) {
    if (w <= 0 || h <= 0) return;
    HLine(x, y, w, gray);
    HLine(x, y + h - 1, w, gray);
    VLine(x, y, h, gray);
    VLine(x + w - 1, y, h, gray);
}

void PixelCanvas::RoundRect(int x, int y, int w, int h, uint8_t gray, int thickness) {
    for (int t = 0; t < thickness; ++t) {
        const int rx = x + t, ry = y + t, rw = w - 2 * t, rh = h - 2 * t;
        if (rw < 6 || rh < 6) {
            Rect(rx, ry, rw, rh, gray);
            continue;
        }
        HLine(rx + 3, ry, rw - 6, gray);
        HLine(rx + 3, ry + rh - 1, rw - 6, gray);
        VLine(rx, ry + 3, rh - 6, gray);
        VLine(rx + rw - 1, ry + 3, rh - 6, gray);
        Pixel(rx + 1, ry + 2, gray); Pixel(rx + 2, ry + 1, gray);
        Pixel(rx + rw - 2, ry + 2, gray); Pixel(rx + rw - 3, ry + 1, gray);
        Pixel(rx + 1, ry + rh - 3, gray); Pixel(rx + 2, ry + rh - 2, gray);
        Pixel(rx + rw - 2, ry + rh - 3, gray); Pixel(rx + rw - 3, ry + rh - 2, gray);
    }
}

void PixelCanvas::FillRoundRect(int x, int y, int w, int h, uint8_t gray) {
    if (w < 6 || h < 6) {
        FillRect(x, y, w, h, gray);
        return;
    }
    FillRect(x + 3, y, w - 6, h, gray);
    FillRect(x + 2, y + 1, w - 4, h - 2, gray);
    FillRect(x + 1, y + 2, w - 2, h - 4, gray);
    FillRect(x, y + 3, w, h - 6, gray);
}

void PixelCanvas::ShadowCard(int x, int y, int w, int h, uint8_t fill_gray,
                             uint8_t border_gray, uint8_t shadow_gray,
                             int offset, int border_w) {
    if (offset > 0) {
        FillRoundRect(x + offset, y + offset, w, h, shadow_gray);
    }
    FillRoundRect(x, y, w, h, fill_gray);
    RoundRect(x, y, w, h, border_gray, border_w);
}

void PixelCanvas::HLine(int x, int y, int len, uint8_t gray) {
    for (int i = 0; i < len; ++i) Pixel(x + i, y, gray);
}

void PixelCanvas::VLine(int x, int y, int len, uint8_t gray) {
    for (int i = 0; i < len; ++i) Pixel(x, y + i, gray);
}

void PixelCanvas::DottedHLine(int x, int y, int len, int step, uint8_t gray) {
    for (int i = 0; i < len; i += step) Pixel(x + i, y, gray);
}

void PixelCanvas::DotGrid(int x0, int y0, int x1, int y1, int step, uint8_t gray) {
    for (int y = y0; y < y1; y += step) {
        for (int x = x0; x < x1; x += step) {
            Pixel(x, y, gray);
        }
    }
}

void PixelCanvas::Circle(int cx, int cy, int r, uint8_t gray, bool fill) {
    for (int dy = -r; dy <= r; ++dy) {
        for (int dx = -r; dx <= r; ++dx) {
            const int d2 = dx * dx + dy * dy;
            if (fill) {
                if (d2 <= r * r) Pixel(cx + dx, cy + dy, gray);
            } else {
                if (d2 <= r * r && d2 >= (r - 1) * (r - 1)) Pixel(cx + dx, cy + dy, gray);
            }
        }
    }
}

bool PixelCanvas::DecodeUtf8(const std::string& s, size_t* idx, uint32_t* cp) {
    if (*idx >= s.size()) return false;
    const uint8_t c0 = static_cast<uint8_t>(s[*idx]);
    if (c0 < 0x80) { *cp = c0; *idx += 1; return true; }
    if ((c0 & 0xE0) == 0xC0 && *idx + 1 < s.size()) {
        *cp = ((c0 & 0x1F) << 6) | (static_cast<uint8_t>(s[*idx + 1]) & 0x3F);
        *idx += 2; return true;
    }
    if ((c0 & 0xF0) == 0xE0 && *idx + 2 < s.size()) {
        *cp = ((c0 & 0x0F) << 12) |
              ((static_cast<uint8_t>(s[*idx + 1]) & 0x3F) << 6) |
              (static_cast<uint8_t>(s[*idx + 2]) & 0x3F);
        *idx += 3; return true;
    }
    if ((c0 & 0xF8) == 0xF0 && *idx + 3 < s.size()) {
        *cp = ((c0 & 0x07) << 18) |
              ((static_cast<uint8_t>(s[*idx + 1]) & 0x3F) << 12) |
              ((static_cast<uint8_t>(s[*idx + 2]) & 0x3F) << 6) |
              (static_cast<uint8_t>(s[*idx + 3]) & 0x3F);
        *idx += 4; return true;
    }
    *cp = '?'; *idx += 1; return true;
}

const PixelCanvas::FontEntry* PixelCanvas::FindGlyph(const LoadedFont& f, uint16_t cp) const {
    if (f.entries == nullptr || f.glyph_count == 0) return nullptr;
    int lo = 0, hi = static_cast<int>(f.glyph_count) - 1;
    while (lo <= hi) {
        const int mid = lo + (hi - lo) / 2;
        const uint16_t mid_cp = f.entries[mid].codepoint;
        if (mid_cp == cp) return &f.entries[mid];
        if (mid_cp < cp) lo = mid + 1;
        else hi = mid - 1;
    }
    return nullptr;
}

int PixelCanvas::DrawCodepoint(int x, int y, uint16_t cp, int scale, uint8_t gray) {
    const LoadedFont& f = SelectFont(scale);
    const FontEntry* g = FindGlyph(f, cp);
    if (g == nullptr) {
        const int fb = (cp < 0x80) ? (f.line_height / 2) : (f.line_height - 1);
        if (cp != ' ') Rect(x + 1, y + 2, fb - 2, f.line_height - 4, gray);
        return fb;
    }
    const uint32_t nbits = static_cast<uint32_t>(g->box_w) * g->box_h;
    const uint32_t nbytes = (nbits + 7) / 8;
    if (g->bmp_offset + nbytes <= f.bitmap_bytes && g->box_w > 0 && g->box_h > 0) {
        const uint8_t* bmp = f.bitmaps + g->bmp_offset;
        const int y_top = y + (static_cast<int>(f.line_height) - static_cast<int>(f.base_line) -
                               static_cast<int>(g->box_h) - static_cast<int>(g->ofs_y));
        uint32_t bit_pos = 0;
        for (int ry = 0; ry < g->box_h; ++ry) {
            for (int rx = 0; rx < g->box_w; ++rx) {
                const uint8_t byte = bmp[bit_pos >> 3];
                const uint8_t bit = (byte >> (7 - (bit_pos & 7))) & 1u;
                bit_pos++;
                if (bit) {
                    Pixel(x + g->ofs_x + rx, y_top + ry, gray);
                }
            }
        }
    }
    return g->adv_w;
}

int PixelCanvas::MeasureText(const std::string& utf8, int scale) const {
    const LoadedFont& f = SelectFont(scale);
    int total = 0;
    size_t idx = 0;
    uint32_t cp = 0;
    while (DecodeUtf8(utf8, &idx, &cp)) {
        if (cp == '\n' || cp == '\r') continue;
        const uint16_t u16 = cp <= 0xFFFF ? static_cast<uint16_t>(cp) : '?';
        const FontEntry* g = FindGlyph(f, u16);
        total += g ? g->adv_w : ((u16 < 0x80) ? (f.line_height / 2) : (f.line_height - 1));
    }
    return total;
}

int PixelCanvas::DrawText(int x, int y, const std::string& utf8, int scale,
                          uint8_t gray, int max_width) {
    const LoadedFont& f = SelectFont(scale);
    int cur = x;
    size_t idx = 0;
    uint32_t cp = 0;
    const int ell_w = MeasureText("..", scale);
    while (DecodeUtf8(utf8, &idx, &cp)) {
        if (cp == '\n' || cp == '\r') continue;
        const uint16_t u16 = cp <= 0xFFFF ? static_cast<uint16_t>(cp) : '?';
        const FontEntry* g = FindGlyph(f, u16);
        const int gw = g ? g->adv_w : ((u16 < 0x80) ? (f.line_height / 2) : (f.line_height - 1));
        if (max_width > 0 && (cur - x) + gw > max_width) {
            if (max_width >= ell_w) DrawText(cur, y, "..", scale, gray, 0);
            break;
        }
        cur += DrawCodepoint(cur, y, u16, scale, gray);
    }
    return cur - x;
}

int PixelCanvas::DrawTextCentered(int y, const std::string& utf8, int scale, uint8_t gray) {
    const int w = MeasureText(utf8, scale);
    return DrawText(std::max(0, (kWidth - w) / 2), y, utf8, scale, gray);
}

int PixelCanvas::DrawWrappedText(int x, int y, int max_width, int max_lines,
                                 int line_height, const std::string& utf8,
                                 int scale, uint8_t gray) {
    if (max_lines <= 0 || max_width <= 0) return 0;
    const LoadedFont& f = SelectFont(scale);
    int line = 0, cx = x;
    size_t idx = 0;
    uint32_t cp = 0;
    while (DecodeUtf8(utf8, &idx, &cp)) {
        if (cp == '\r') continue;
        if (cp == '\n') {
            if (++line >= max_lines) break;
            cx = x;
            continue;
        }
        const uint16_t u16 = cp <= 0xFFFF ? static_cast<uint16_t>(cp) : '?';
        const FontEntry* g = FindGlyph(f, u16);
        const int gw = g ? g->adv_w : ((u16 < 0x80) ? (f.line_height / 2) : (f.line_height - 1));
        if (cx > x && (cx - x) + gw > max_width) {
            if (++line >= max_lines) break;
            cx = x;
        }
        cx += DrawCodepoint(cx, y + line * line_height, u16, scale, gray);
    }
    return line + 1;
}

void PixelCanvas::DrawMascot(int x, int y, bool urgent_face) {
    // Ground shadow + tiny feet
    FillRoundRect(x + 4, y + 38, 28, 4, 12);
    FillRoundRect(x + 6, y + 35, 6, 5, 2);
    FillRoundRect(x + 24, y + 35, 6, 5, 2);
    // Cute fridge body (32x35)
    FillRoundRect(x + 2, y + 2, 32, 35, 14);
    RoundRect(x + 2, y + 2, 32, 35, 1, 2);
    HLine(x + 4, y + 24, 28, 4);
    // Door handles
    FillRect(x + 28, y + 16, 2, 5, 2);
    FillRect(x + 28, y + 27, 2, 5, 2);
    // Eyes
    FillRect(x + 9, y + 10, 3, 4, 0);
    FillRect(x + 21, y + 10, 3, 4, 0);
    Pixel(x + 10, y + 11, 15);
    Pixel(x + 22, y + 11, 15);
    // Rosy gray cheeks
    FillRect(x + 6, y + 15, 4, 2, 11);
    FillRect(x + 23, y + 15, 4, 2, 11);
    // Mouth: cute 'o' if urgent reminder, smiling curve if all fresh!
    if (urgent_face) {
        Rect(x + 15, y + 15, 4, 4, 0);
    } else {
        HLine(x + 14, y + 17, 6, 0);
        Pixel(x + 13, y + 16, 0);
        Pixel(x + 20, y + 16, 0);
    }
    DrawSparkle(x + 37, y + 4, 3, 4);
}

void PixelCanvas::DrawFoodIcon(FoodIcon icon, int x, int y, uint8_t gray) {
    switch (icon) {
        case FoodIcon::kMilk:
            HLine(x + 3, y, 5, gray);
            Pixel(x + 2, y + 1, gray); Pixel(x + 8, y + 1, gray);
            Pixel(x + 1, y + 2, gray); Pixel(x + 9, y + 2, gray);
            Rect(x + 1, y + 3, 9, 8, gray);
            HLine(x + 3, y + 6, 5, gray);
            break;
        case FoodIcon::kLeaf:
            Circle(x + 5, y + 5, 4, gray, false);
            for (int i = 0; i < 6; ++i) Pixel(x + 2 + i, y + 9 - i, gray);
            break;
        case FoodIcon::kEgg:
            RoundRect(x + 2, y + 1, 7, 10, gray, 1);
            Pixel(x + 4, y + 4, gray);
            break;
        case FoodIcon::kMeat:
            RoundRect(x + 1, y + 2, 9, 8, gray, 1);
            Rect(x + 4, y + 4, 3, 4, gray);
            break;
        case FoodIcon::kSnow:
            HLine(x + 1, y + 5, 9, gray);
            VLine(x + 5, y + 1, 9, gray);
            for (int i = -3; i <= 3; ++i) {
                Pixel(x + 5 + i, y + 5 + i, gray);
                Pixel(x + 5 + i, y + 5 - i, gray);
            }
            break;
        case FoodIcon::kFruit:
            Circle(x + 5, y + 6, 4, gray, false);
            Pixel(x + 5, y + 1, gray); Pixel(x + 6, y, gray);
            break;
        case FoodIcon::kDrink:
            Rect(x + 2, y + 3, 7, 8, gray);
            Pixel(x + 6, y + 2, gray); Pixel(x + 7, y + 1, gray); Pixel(x + 8, y, gray);
            break;
    }
}

void PixelCanvas::DrawPanIcon(int x, int y, uint8_t gray) {
    RoundRect(x + 1, y + 5, 13, 7, gray, 1);
    HLine(x, y + 5, 15, gray);
    HLine(x + 5, y + 3, 5, gray);
    Pixel(x + 5, y, gray); Pixel(x + 6, y + 1, gray);
    Pixel(x + 9, y, gray); Pixel(x + 10, y + 1, gray);
}

void PixelCanvas::DrawBasketIcon(int x, int y, uint8_t gray) {
    HLine(x + 1, y + 5, 12, gray);
    HLine(x + 3, y + 11, 8, gray);
    for (int i = 0; i < 6; ++i) {
        Pixel(x + 1 + i / 3, y + 5 + i, gray);
        Pixel(x + 12 - i / 3, y + 5 + i, gray);
    }
    HLine(x + 4, y + 2, 6, gray);
    VLine(x + 3, y + 3, 2, gray); VLine(x + 10, y + 3, 2, gray);
    VLine(x + 5, y + 7, 3, gray); VLine(x + 8, y + 7, 3, gray);
}

void PixelCanvas::DrawMicIcon(int x, int y, uint8_t gray) {
    FillRoundRect(x + 3, y, 5, 7, gray);
    VLine(x + 1, y + 4, 4, gray);
    VLine(x + 9, y + 4, 4, gray);
    HLine(x + 2, y + 8, 7, gray);
    VLine(x + 5, y + 9, 3, gray);
    HLine(x + 3, y + 11, 5, gray);
}

void PixelCanvas::DrawWifiIcon(int x, int y, bool connected, uint8_t gray) {
    if (!connected) {
        DrawCross(x + 2, y + 2, gray);
        return;
    }
    HLine(x + 2, y + 2, 7, gray);
    Pixel(x + 1, y + 3, gray); Pixel(x + 9, y + 3, gray);
    HLine(x + 3, y + 5, 5, gray);
    FillRect(x + 5, y + 8, 2, 2, gray);
}

void PixelCanvas::DrawBatteryIcon(int x, int y, int pct, bool charging, uint8_t fg, uint8_t bg) {
    FillRect(x, y + 1, 19, 10, bg);
    Rect(x, y + 1, 19, 10, fg);
    FillRect(x + 19, y + 4, 2, 4, fg);
    const int fw = std::clamp(pct * 15 / 100, 1, 15);
    FillRect(x + 2, y + 3, fw, 6, fg);
    if (charging) {
        Pixel(x + 9, y + 4, bg); Pixel(x + 8, y + 5, bg);
        Pixel(x + 9, y + 6, bg); Pixel(x + 10, y + 6, bg); Pixel(x + 9, y + 7, bg);
    }
}

void PixelCanvas::DrawCheck(int x, int y, uint8_t gray) {
    for (int i = 0; i < 3; ++i) {
        Pixel(x + i, y + 4 + i, gray);
        Pixel(x + i, y + 5 + i, gray);
    }
    for (int i = 0; i < 5; ++i) {
        Pixel(x + 3 + i, y + 6 - i, gray);
        Pixel(x + 3 + i, y + 7 - i, gray);
    }
}

void PixelCanvas::DrawCross(int x, int y, uint8_t gray) {
    for (int i = 0; i < 7; ++i) {
        Pixel(x + i, y + i, gray);
        Pixel(x + 6 - i, y + i, gray);
    }
}

void PixelCanvas::DrawArrowRight(int x, int y, uint8_t gray) {
    HLine(x, y + 4, 8, gray);
    HLine(x, y + 5, 8, gray);
    Pixel(x + 5, y + 2, gray); Pixel(x + 6, y + 3, gray);
    Pixel(x + 6, y + 6, gray); Pixel(x + 5, y + 7, gray);
}

void PixelCanvas::DrawArrowLeft(int x, int y, uint8_t gray) {
    HLine(x, y + 4, 8, gray);
    HLine(x, y + 5, 8, gray);
    Pixel(x + 2, y + 2, gray); Pixel(x + 1, y + 3, gray);
    Pixel(x + 1, y + 6, gray); Pixel(x + 2, y + 7, gray);
}

void PixelCanvas::DrawPlus(int x, int y, uint8_t gray) {
    HLine(x, y + 4, 9, gray);
    VLine(x + 4, y, 9, gray);
}

void PixelCanvas::DrawClockIcon(int x, int y, uint8_t gray) {
    Circle(x + 4, y + 4, 4, gray, false);
    VLine(x + 4, y + 2, 3, gray);
    HLine(x + 4, y + 4, 3, gray);
}

void PixelCanvas::DrawPointer(int x, int y, uint8_t gray) {
    for (int i = 0; i < 5; ++i) {
        VLine(x + i, y + i, 9 - 2 * i, gray);
    }
}

void PixelCanvas::DrawSparkle(int cx, int cy, int r, uint8_t gray) {
    HLine(cx - r, cy, 2 * r + 1, gray);
    VLine(cx, cy - r, 2 * r + 1, gray);
}

void PixelCanvas::DrawUpDownArrows(int x, int y, uint8_t gray) {
    VLine(x + 2, y, 10, gray); VLine(x + 3, y, 10, gray);
    Pixel(x + 1, y + 1, gray); Pixel(x + 4, y + 1, gray);
    Pixel(x, y + 2, gray); Pixel(x + 5, y + 2, gray);
    VLine(x + 10, y, 10, gray); VLine(x + 11, y, 10, gray);
    Pixel(x + 9, y + 8, gray); Pixel(x + 12, y + 8, gray);
    Pixel(x + 8, y + 7, gray); Pixel(x + 13, y + 7, gray);
}

bool PixelCanvas::DrawQrCode(int x, int y, int max_size_px, const std::string& text) {
    if (text.empty()) return false;
    uint8_t qr[qrcodegen_BUFFER_LEN_MAX];
    uint8_t tmp[qrcodegen_BUFFER_LEN_MAX];
    const bool ok = qrcodegen_encodeText(
        text.c_str(), tmp, qr, qrcodegen_Ecc_LOW,
        qrcodegen_VERSION_MIN, 10, qrcodegen_Mask_AUTO, true);
    if (!ok) return false;
    const int size = qrcodegen_getSize(qr);
    const int scale = std::max(1, max_size_px / size);
    const int total = size * scale;
    const int ox = x + (max_size_px - total) / 2;
    const int oy = y + (max_size_px - total) / 2;
    FillRect(ox - 2, oy - 2, total + 4, total + 4, 15);
    for (int r = 0; r < size; ++r) {
        for (int c = 0; c < size; ++c) {
            if (qrcodegen_getModule(qr, c, r)) {
                FillRect(ox + c * scale, oy + r * scale, scale, scale, 0);
            }
        }
    }
    return true;
}

void PixelCanvas::ExtractPatch1bpp(const zectrix_epd_rect_t& rect,
                                   zectrix_epd_rect_t* aligned_rect,
                                   std::vector<uint8_t>* out_patch) const {
    const int x0 = std::max(0, rect.x & ~7);
    const int y0 = std::max(0, rect.y);
    const int x1 = std::min(kWidth, ((rect.x + rect.width + 7) & ~7));
    const int y1 = std::min(kHeight, rect.y + rect.height);
    const int w = std::max(8, x1 - x0);
    const int h = std::max(1, y1 - y0);
    aligned_rect->x = x0;
    aligned_rect->y = y0;
    aligned_rect->width = w;
    aligned_rect->height = h;
    const int stride = w / 8;
    out_patch->assign(static_cast<size_t>(stride) * h, 0xFF);
    const int byte_off = x0 / 8;
    for (int row = 0; row < h; ++row) {
        std::memcpy(out_patch->data() + static_cast<size_t>(row) * stride,
                    buf1_.data() + static_cast<size_t>(y0 + row) * kStride1bpp + byte_off,
                    static_cast<size_t>(stride));
    }
}

// ============================================================================
// Warm-kitchen design primitives (rounded shapes, tablecloth fills, icons, mascot)
// ============================================================================
namespace {

// Returns the gray for a decorative fill at (x, y). In 1bpp mode only 0 / 15 come back.
uint8_t FillGrayAt(UiFill fill, int x, int y, bool four_bpp) {
    switch (fill) {
        case UiFill::kDots25:
            if (four_bpp) return 13;
            return ((x & 1) == 0 && (y & 1) == 0) ? 0 : 15;
        case UiFill::kChecker50:
            if (four_bpp) return 9;
            return ((x + y) & 1) ? 0 : 15;
        case UiFill::kGingham: {
            constexpr int kCell = 6;
            const bool band_x = ((x / kCell) & 1) != 0;
            const bool band_y = ((y / kCell) & 1) != 0;
            if (four_bpp) return (band_x && band_y) ? 10 : ((band_x || band_y) ? 13 : 15);
            if (band_x && band_y) return ((x + y) & 1) ? 0 : 15;              // 50% where stripes cross
            if (band_x || band_y) return ((x & 1) == 0 && (y & 1) == 0) ? 0 : 15;  // 25% single stripe
            return 15;
        }
    }
    return 15;
}

// Horizontal inset of a rounded-rect row (dy measured from the nearest top/bottom edge).
int CornerInset(int r, int dy) {
    if (r <= 0 || dy >= r) return 0;
    const int yy2 = (2 * (r - dy) - 1) * (2 * (r - dy) - 1);  // (2*(r-dy)-1)^2 == (2*yy)^2
    const int rr4 = 4 * r * r;
    int inset = 0;
    // smallest inset where the pixel centre lies inside the corner circle
    while (inset < r) {
        const int xx = 2 * (r - inset) - 1;
        if (xx * xx + yy2 <= rr4) break;
        ++inset;
    }
    return inset;
}

template <typename SpanFn>
void ForEachRoundRectSpan(int x, int y, int w, int h, int r, SpanFn&& span) {
    if (w <= 0 || h <= 0) return;
    r = std::max(0, std::min(r, std::min(w, h) / 2));
    for (int row = 0; row < h; ++row) {
        const int dy = std::min(row, h - 1 - row);
        const int inset = CornerInset(r, dy);
        span(x + inset, y + row, w - 2 * inset);
    }
}

// 16x16 icon bitmaps: '#' = ink, anything else = transparent
constexpr const char* kIcons[static_cast<int>(UiIcon::kCount)][16] = {
    {   // kMilk
        "......####......",
        ".....#....#.....",
        "....#......#....",
        "...##########...",
        "...#........#...",
        "...#.######.#...",
        "...#.#....#.#...",
        "...#.#.##.#.#...",
        "...#.#.##.#.#...",
        "...#.#....#.#...",
        "...#.######.#...",
        "...#........#...",
        "...#........#...",
        "...##########...",
        "................",
        "................",
    },
    {   // kLeaf
        "................",
        "..........#####.",
        "........##....#.",
        "......##......#.",
        ".....#.......#..",
        "....#...#....#..",
        "...#...#....#...",
        "...#..#....#....",
        "..#..#....#.....",
        "..#.#....#......",
        "..##...##.......",
        "..#.###.........",
        ".#..............",
        "#...............",
        "................",
        "................",
    },
    {   // kMeat (drumstick)
        "................",
        ".......#####....",
        ".....##.....#...",
        "....#........#..",
        "...#..##......#.",
        "...#.#........#.",
        "...#..........#.",
        "....#........#..",
        ".....#......#...",
        "......#....#....",
        ".....##.###.....",
        "...##..#........",
        "..#...#.........",
        "..#..#..........",
        "...##...........",
        "................",
    },
    {   // kEgg
        "................",
        "......####......",
        ".....#....#.....",
        "....#......#....",
        "...#..##....#...",
        "...#.#......#...",
        "..#..#.......#..",
        "..#..........#..",
        "..#..........#..",
        "..#..........#..",
        "..#..........#..",
        "...#........#...",
        "....#......#....",
        ".....######.....",
        "................",
        "................",
    },
    {   // kFruit (apple)
        "................",
        "........#..##...",
        ".......#..#..#..",
        ".......#...##...",
        "...#####.####...",
        "..#....#.....#..",
        ".#..##........#.",
        ".#.#..........#.",
        ".#............#.",
        ".#............#.",
        "..#..........#..",
        "..#..........#..",
        "...#...##...#...",
        "....###..###....",
        "................",
        "................",
    },
    {   // kDrink (cup with straw)
        ".........###....",
        ".........#......",
        ".........#......",
        "..###########...",
        "..#......#..#...",
        "..###########...",
        "...#.....#.#....",
        "...#.....#.#....",
        "...#.#.#.#.#....",
        "...#.#.#.#.#....",
        "....#.....#.....",
        "....#.....#.....",
        "....#######.....",
        "................",
        "................",
        "................",
    },
    {   // kSnow
        ".......#........",
        ".....#.#.#......",
        "......###.......",
        ".#.....#.....#..",
        "..#....#....#...",
        "#..#...#...#..#.",
        ".###...#...###..",
        "....#..#..#.....",
        "#######.#######.",
        "....#..#..#.....",
        ".###...#...###..",
        "#..#...#...#..#.",
        "..#....#....#...",
        ".#....###....#..",
        ".....#.#.#......",
        ".......#........",
    },
    {   // kPot
        "................",
        "......####......",
        ".......##.......",
        "...##########...",
        "..#..........#..",
        "################",
        "#.#..........#.#",
        "..#..........#..",
        "..#..........#..",
        "..#..........#..",
        "..#..........#..",
        "...#........#...",
        "....########....",
        "................",
        "................",
        "................",
    },
    {   // kBasket
        "................",
        "......####......",
        ".....#....#.....",
        "....#......#....",
        "...#........#...",
        "################",
        "#..............#",
        ".#..#..##..#..#.",
        ".#..#..##..#..#.",
        ".#..#..##..#..#.",
        "..#..#.##.#..#..",
        "..#..#.##.#..#..",
        "...##########...",
        "................",
        "................",
        "................",
    },
    {   // kClock
        "................",
        ".....######.....",
        "...##......##...",
        "..#....#.....#..",
        "..#....#.....#..",
        ".#.....#......#.",
        ".#.....#......#.",
        ".#.....####...#.",
        ".#............#.",
        ".#............#.",
        "..#..........#..",
        "..#..........#..",
        "...##......##...",
        ".....######.....",
        "................",
        "................",
    },
    {   // kMic
        "......####......",
        ".....#....#.....",
        ".....#....#.....",
        ".....#....#.....",
        ".....#....#.....",
        "..#..#....#..#..",
        "..#..#....#..#..",
        "..#...####...#..",
        "...#........#...",
        "....##....##....",
        "......####......",
        ".......##.......",
        ".......##.......",
        ".....######.....",
        "................",
        "................",
    },
    {   // kWifi
        "................",
        "................",
        "....########....",
        "..##........##..",
        ".#............#.",
        "#....######....#",
        "...##......##...",
        "..#..........#..",
        ".....######.....",
        "....#......#....",
        "................",
        "......####......",
        "......####......",
        "................",
        "................",
        "................",
    },
    {   // kMoon (filled crescent + star)
        "................",
        ".....####.......",
        "...#####......#.",
        "..#####......###",
        ".#####........#.",
        ".####...........",
        "#####...........",
        "#####...........",
        "#####...........",
        "######.........#",
        ".######.......##",
        ".#######....###.",
        "..############..",
        "....########....",
        "................",
        "................",
    },
    {   // kPower
        ".......##.......",
        ".......##.......",
        "...#...##...#...",
        "..##...##...##..",
        ".##....##....##.",
        ".#.....##.....#.",
        "#......##......#",
        "#..............#",
        "#..............#",
        "#..............#",
        ".#............#.",
        ".##..........##.",
        "..###......###..",
        "....########....",
        "................",
        "................",
    },
    {   // kBell
        ".......##.......",
        ".....######.....",
        "....#......#....",
        "...#........#...",
        "...#........#...",
        "...#........#...",
        "...#........#...",
        "...#........#...",
        "..#..........#..",
        "..#..........#..",
        ".#............#.",
        "################",
        "................",
        "......####......",
        ".......##.......",
        "................",
    },
    {   // kFridge
        "..############..",
        ".#............#.",
        ".#.........##.#.",
        ".#.........##.#.",
        ".#............#.",
        ".#............#.",
        ".##############.",
        ".#............#.",
        ".#.........##.#.",
        ".#.........##.#.",
        ".#.........##.#.",
        ".#............#.",
        ".#............#.",
        "..############..",
        "..##........##..",
        "................",
    },
    {   // kRefresh
        "................",
        ".....#####..#...",
        "...##.....###...",
        "..#........##...",
        ".#........###...",
        ".#..............",
        "#...............",
        "#..............#",
        "...............#",
        "..............#.",
        "...###........#.",
        "...##........#..",
        "...###.....##...",
        "...#..#####.....",
        "................",
        "................",
    },
    {   // kHeart
        "................",
        "..####....####..",
        ".#....#..#....#.",
        "#......##......#",
        "#..##..........#",
        "#.#............#",
        "#..............#",
        ".#............#.",
        "..#..........#..",
        "...#........#...",
        "....#......#....",
        ".....#....#.....",
        "......#..#......",
        ".......##.......",
        "................",
        "................",
    },
    {   // kCheck
        "................",
        "................",
        "..............##",
        ".............###",
        "............###.",
        "...........###..",
        "..........###...",
        "##.......###....",
        "###.....###.....",
        ".###...###......",
        "..###.###.......",
        "...#####........",
        "....###.........",
        ".....#..........",
        "................",
        "................",
    },
    {   // kCross
        "................",
        "................",
        "..##........##..",
        "..###......###..",
        "...###....###...",
        "....###..###....",
        ".....######.....",
        "......####......",
        "......####......",
        ".....######.....",
        "....###..###....",
        "...###....###...",
        "..###......###..",
        "..##........##..",
        "................",
        "................",
    },
    {   // kFlame
        ".......#........",
        ".......##.......",
        "......#.#.......",
        "......#..#......",
        ".....#...#..#...",
        "....#.....#.##..",
        "...#..........#.",
        "...#....#.....#.",
        "..#....#.#.....#",
        "..#...#...#....#",
        "..#..#.....#...#",
        "..#..#.....#...#",
        "...#..#...#...#.",
        "....##.....###..",
        "......#####.....",
        "................",
    },
    {   // kSpark (sparkle)
        ".......#........",
        ".......#........",
        "......###.......",
        "......###.......",
        ".....#####......",
        "..###########...",
        "#######.#######.",
        "..###########...",
        ".....#####......",
        "......###.......",
        "......###.......",
        ".......#........",
        ".......#........",
        "................",
        "................",
        "................",
    },
};

// Mascot pixel art (20x20). '#' outline, 'c' blush, 'w' eye shine, 's' ground shadow,
// '.' body (light), ' ' transparent. Rows 8-10 hold the mouth and are swapped for moods.
constexpr const char* kMascot[20] = {
    "  ################  ",
    " #................# ",
    "#..................#",
    "#..................#",
    "#...###......###...#",
    "#...#w#......#w#...#",
    "#...###......###...#",
    "#..................#",
    "#.cc...#....#...cc.#",
    "#.cc....####....cc.#",
    "#..................#",
    "####################",
    "#...............##.#",
    "#...............##.#",
    "#...............##.#",
    "#..................#",
    " #................# ",
    "  ################  ",
    "   ###        ###   ",
    "  ssssssssssssssss  ",
};
constexpr const char* kMascotWorried[3] = {
    "#.cc.....##.....cc.#",
    "#.cc....#..#....cc.#",
    "#.........##.......#",
};

}  // namespace

void PixelCanvas::FillRoundRectR(int x, int y, int w, int h, int r, uint8_t gray) {
    ForEachRoundRectSpan(x, y, w, h, r, [&](int sx, int sy, int sw) { HLine(sx, sy, sw, gray); });
}

void PixelCanvas::RoundRectR(int x, int y, int w, int h, int r, uint8_t gray, int thickness) {
    if (w <= 0 || h <= 0 || thickness <= 0) return;
    r = std::max(0, std::min(r, std::min(w, h) / 2));
    const int t = std::min(thickness, std::min(w, h) / 2);
    const int ir = std::max(0, r - t);
    for (int row = 0; row < h; ++row) {
        const int dy = std::min(row, h - 1 - row);
        const int outer = CornerInset(r, dy);
        const int ox0 = x + outer, ox1 = x + w - outer;  // [ox0, ox1)
        const int irow = row - t;
        if (irow < 0 || irow >= h - 2 * t) {
            HLine(ox0, y + row, ox1 - ox0, gray);
            continue;
        }
        const int idy = std::min(irow, h - 2 * t - 1 - irow);
        const int inner = CornerInset(ir, idy);
        const int ix0 = x + t + inner, ix1 = x + w - t - inner;
        HLine(ox0, y + row, std::max(0, ix0 - ox0), gray);
        HLine(ix1, y + row, std::max(0, ox1 - ix1), gray);
    }
}

void PixelCanvas::FillPattern(int x, int y, int w, int h, UiFill fill) {
    for (int py = std::max(0, y); py < std::min(kHeight, y + h); ++py) {
        for (int px = std::max(0, x); px < std::min(kWidth, x + w); ++px) {
            Pixel(px, py, FillGrayAt(fill, px, py, mode_4bpp_));
        }
    }
}

void PixelCanvas::FillPatternRoundRectR(int x, int y, int w, int h, int r, UiFill fill) {
    ForEachRoundRectSpan(x, y, w, h, r, [&](int sx, int sy, int sw) {
        for (int px = sx; px < sx + sw; ++px) Pixel(px, sy, FillGrayAt(fill, px, sy, mode_4bpp_));
    });
}

void PixelCanvas::DrawIcon16(UiIcon icon, int x, int y, uint8_t gray) {
    const int idx = static_cast<int>(icon);
    if (idx < 0 || idx >= static_cast<int>(UiIcon::kCount)) return;
    for (int ry = 0; ry < 16; ++ry) {
        const char* row = kIcons[idx][ry];
        for (int rx = 0; rx < 16 && row[rx] != '\0'; ++rx) {
            if (row[rx] == '#') Pixel(x + rx, y + ry, gray);
        }
    }
}

UiIcon PixelCanvas::IconForFood(FoodIcon food) {
    switch (food) {
        case FoodIcon::kMilk: return UiIcon::kMilk;
        case FoodIcon::kLeaf: return UiIcon::kLeaf;
        case FoodIcon::kMeat: return UiIcon::kMeat;
        case FoodIcon::kEgg: return UiIcon::kEgg;
        case FoodIcon::kFruit: return UiIcon::kFruit;
        case FoodIcon::kDrink: return UiIcon::kDrink;
        case FoodIcon::kSnow: return UiIcon::kSnow;
    }
    return UiIcon::kLeaf;
}

void PixelCanvas::DrawMascotPixel(int x, int y, int scale, bool worried) {
    scale = std::max(1, scale);
    for (int ry = 0; ry < 20; ++ry) {
        const char* row = (worried && ry >= 8 && ry <= 10) ? kMascotWorried[ry - 8] : kMascot[ry];
        for (int rx = 0; rx < 20 && row[rx] != '\0'; ++rx) {
            const char c = row[rx];
            if (c == ' ') continue;
            for (int sy = 0; sy < scale; ++sy) {
                for (int sx = 0; sx < scale; ++sx) {
                    const int px = x + rx * scale + sx;
                    const int py = y + ry * scale + sy;
                    uint8_t g = 15;
                    switch (c) {
                        case '#': g = 0; break;
                        case 'w': g = 15; break;
                        case '.': g = mode_4bpp_ ? 14 : 15; break;
                        case 'c': g = mode_4bpp_ ? 10 : (((px + py) & 1) ? 0 : 15); break;
                        case 's': g = mode_4bpp_ ? 12 : (((px & 1) == 0 && (py & 1) == 0) ? 0 : 15); break;
                        default: break;
                    }
                    Pixel(px, py, g);
                }
            }
        }
    }
}
