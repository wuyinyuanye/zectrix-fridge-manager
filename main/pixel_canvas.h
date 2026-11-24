#ifndef ZECTRIX_FRIDGE_PIXEL_CANVAS_H_
#define ZECTRIX_FRIDGE_PIXEL_CANVAS_H_

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "food_catalog.h"
#include "zectrix_epd.h"

// 16x16 pixel-art icon set ("warm kitchen" style), drawn with a single ink gray
enum class UiIcon : uint8_t {
    kMilk = 0, kLeaf, kMeat, kEgg, kFruit, kDrink, kSnow,
    kPot, kBasket, kClock, kMic, kWifi, kMoon, kPower, kBell,
    kFridge, kRefresh, kHeart, kCheck, kCross, kFlame, kSpark,
    kCount,
};

// Decorative fills. 1bpp: ordered dither; 4bpp: real gray tones.
enum class UiFill : uint8_t {
    kDots25 = 0,   // light tint (sparse dots)
    kChecker50,    // mid tint / soft shadow
    kGingham,      // kitchen tablecloth check
};

// Dual-mode canvas for ZECTRIX NOTE4 400x300 EPD:
// - 1bpp buffer (15,000 bytes) for fast interactive pages (Pages 1-5)
// - 4bpp buffer (60,000 bytes, 16 gray levels 0=black..15=white) for Page 0 Standby Poster
class PixelCanvas {
public:
    static constexpr int kWidth = ZECTRIX_EPD_PANEL_WIDTH;    // 400
    static constexpr int kHeight = ZECTRIX_EPD_PANEL_HEIGHT;  // 300
    static constexpr int kStride1bpp = kWidth / 8;            // 50
    static constexpr int kStride4bpp = kWidth / 2;            // 200

    PixelCanvas();

    void SetMode4bpp(bool enable_4bpp);
    bool is_4bpp() const { return mode_4bpp_; }

    // gray: 0 (black) .. 15 (white). In 1bpp mode, <=7 is black, >=8 is white.
    void Clear(uint8_t gray = 15);
    void Pixel(int x, int y, uint8_t gray);
    void FillRect(int x, int y, int w, int h, uint8_t gray);
    void Rect(int x, int y, int w, int h, uint8_t gray = 0);
    void RoundRect(int x, int y, int w, int h, uint8_t gray = 0, int thickness = 1);
    void FillRoundRect(int x, int y, int w, int h, uint8_t gray);
    void ShadowCard(int x, int y, int w, int h, uint8_t fill_gray,
                    uint8_t border_gray, uint8_t shadow_gray,
                    int offset = 2, int border_w = 1);
    void HLine(int x, int y, int len, uint8_t gray = 0);
    void VLine(int x, int y, int len, uint8_t gray = 0);
    void DottedHLine(int x, int y, int len, int step = 3, uint8_t gray = 0);
    void DotGrid(int x0, int y0, int x1, int y1, int step, uint8_t gray);
    void Circle(int cx, int cy, int r, uint8_t gray, bool fill = false);

    // Official 3.7.5 native dot-matrix fonts (scale=1 -> 17px body, scale>=2 -> 29px title)
    int MeasureText(const std::string& utf8, int scale = 1) const;
    int DrawText(int x, int y, const std::string& utf8, int scale = 1,
                 uint8_t gray = 0, int max_width = 0);
    int DrawTextCentered(int y, const std::string& utf8, int scale = 1,
                         uint8_t gray = 0);
    int DrawWrappedText(int x, int y, int max_width, int max_lines,
                        int line_height, const std::string& utf8,
                        int scale = 1, uint8_t gray = 0);

    // Cheerful pixel-art icons & mascot
    void DrawMascot(int x, int y, bool urgent_face);
    void DrawFoodIcon(FoodIcon icon, int x, int y, uint8_t gray = 0);
    void DrawPanIcon(int x, int y, uint8_t gray = 0);
    void DrawBasketIcon(int x, int y, uint8_t gray = 0);
    void DrawMicIcon(int x, int y, uint8_t gray = 0);
    void DrawWifiIcon(int x, int y, bool connected, uint8_t gray = 0);
    void DrawBatteryIcon(int x, int y, int pct, bool charging, uint8_t fg, uint8_t bg);
    void DrawCheck(int x, int y, uint8_t gray = 0);
    void DrawCross(int x, int y, uint8_t gray = 0);
    void DrawArrowRight(int x, int y, uint8_t gray = 0);
    void DrawArrowLeft(int x, int y, uint8_t gray = 0);
    void DrawPlus(int x, int y, uint8_t gray = 0);
    void DrawClockIcon(int x, int y, uint8_t gray = 0);
    void DrawPointer(int x, int y, uint8_t gray = 0);
    void DrawSparkle(int cx, int cy, int r, uint8_t gray = 0);
    void DrawUpDownArrows(int x, int y, uint8_t gray = 0);

    // ---- Warm-kitchen design primitives ----
    // Rounded rectangles with arbitrary corner radius r
    void FillRoundRectR(int x, int y, int w, int h, int r, uint8_t gray);
    void RoundRectR(int x, int y, int w, int h, int r, uint8_t gray = 0, int thickness = 1);
    void FillPatternRoundRectR(int x, int y, int w, int h, int r, UiFill fill);
    void FillPattern(int x, int y, int w, int h, UiFill fill);
    void DrawIcon16(UiIcon icon, int x, int y, uint8_t gray = 0);
    static UiIcon IconForFood(FoodIcon food);
    // Chunky pixel-art fridge mascot: 20x20 cells * scale (scale 2 == 40px)
    void DrawMascotPixel(int x, int y, int scale, bool worried);

    // Native scannable QR code rendering
    bool DrawQrCode(int x, int y, int max_size_px, const std::string& text);

    // Partial refresh helper (1bpp)
    void ExtractPatch1bpp(const zectrix_epd_rect_t& rect,
                          zectrix_epd_rect_t* aligned_rect,
                          std::vector<uint8_t>* out_patch) const;

    const uint8_t* data1bpp() const { return buf1_.data(); }
    size_t size1bpp() const { return buf1_.size(); }
    const uint8_t* data4bpp() const { return buf4_.data(); }
    size_t size4bpp() const { return buf4_.size(); }

private:
    struct __attribute__((packed)) FontEntry {
        uint16_t codepoint;
        uint8_t adv_w;
        uint8_t box_w;
        uint8_t box_h;
        int8_t ofs_x;
        int8_t ofs_y;
        uint8_t reserved;
        uint32_t bmp_offset;
    };

    struct LoadedFont {
        const FontEntry* entries = nullptr;
        const uint8_t* bitmaps = nullptr;
        uint32_t bitmap_bytes = 0;
        uint16_t glyph_count = 0;
        uint8_t line_height = 17;
        uint8_t base_line = 3;
    };

    static void LoadFont(const uint8_t* start, const uint8_t* end, LoadedFont* out);
    static bool DecodeUtf8(const std::string& s, size_t* idx, uint32_t* cp);
    const LoadedFont& SelectFont(int scale) const { return scale >= 2 ? font29_ : font17_; }
    const FontEntry* FindGlyph(const LoadedFont& f, uint16_t cp) const;
    int DrawCodepoint(int x, int y, uint16_t cp, int scale, uint8_t gray);

    bool mode_4bpp_ = false;
    std::array<uint8_t, ZECTRIX_EPD_1BPP_FRAME_BYTES> buf1_ = {};
    std::vector<uint8_t> buf4_;  // Allocated in PSRAM/heap (60,000 bytes)
    LoadedFont font17_;
    LoadedFont font29_;
};

#endif  // ZECTRIX_FRIDGE_PIXEL_CANVAS_H_
