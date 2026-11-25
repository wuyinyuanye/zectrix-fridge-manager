#include "fridge_ui.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <ctime>

#include "standby_aa_fonts.h"

// ============================================================================
// Dual full-system visual engine for the 400x300 EPD:
//  - UiTheme::kMinimal (default): MUJI / Apple / Kindle editorial minimalism
//  - UiTheme::kCard: Warm kitchen rounded-card stationery with pixel food icons
// ============================================================================

namespace {

constexpr int kMaxPartialBeforeFull = 15;
constexpr int kMaxPillPartialBeforeFull = 30;

// Region owned by the timer-pill partial refresh (must match DrawStatusArea's footprint)
constexpr int kStatusX = 288;
constexpr int kStatusW = 112;
constexpr int kStatusH = 28;

constexpr int kLegendY = 266;

enum class UrgencyLevel : uint8_t {
    kNone = 0,
    kSoon = 1,      // 1-2 days left
    kCritical = 2,  // expired or due today (<= 0)
};

// Truthful date tag: NEVER fabricate fake expiry dates!
// Uses FridgeStore helpers so unsynced-clock relative ages and synced dates match 100%.
std::string FormatItemDateTag(const FridgeItem& it, int32_t today,
                              UrgencyLevel* urgency_out = nullptr) {
    bool has_exp = false;
    const int left = FridgeStore::ComputeItemDaysLeft(it, today, &has_exp);
    if (has_exp) {
        if (urgency_out) {
            *urgency_out = (left <= 0) ? UrgencyLevel::kCritical
                                       : ((left <= 2) ? UrgencyLevel::kSoon : UrgencyLevel::kNone);
        }
        if (left < 0) return "过期" + std::to_string(-left) + "天";
        if (left == 0) return "今天到期";
        return "剩" + std::to_string(left) + "天";
    }
    if (urgency_out) *urgency_out = UrgencyLevel::kNone;
    const int age = FridgeStore::ComputeItemAgeDays(it, today);
    if (age == 0) return "今天放入";
    return "已放" + std::to_string(age) + "天";
}

// Nibble-packed 4bpp anti-aliased numeral typography helpers (standby_aa_fonts.h)
int MeasureAa(const standby_aa::Font& f, const std::string& s, int tracking = 0) {
    int w = 0;
    for (size_t i = 0; i < s.size(); ++i) {
        for (int g = 0; g < f.count; ++g) {
            if (f.glyphs[g].ch == s[i]) {
                w += f.glyphs[g].adv + (i + 1 < s.size() ? tracking : 0);
                break;
            }
        }
    }
    return w;
}

void DrawAaText(PixelCanvas& c, const standby_aa::Font& f, int x, int y, const std::string& s,
                uint8_t fg, int tracking = 0, int colon_y_shift = 0) {
    int cx = x;
    for (size_t i = 0; i < s.size(); ++i) {
        for (int g = 0; g < f.count; ++g) {
            const auto& gl = f.glyphs[g];
            if (gl.ch != s[i]) continue;
            const int dy = (gl.ch == ':') ? colon_y_shift : 0;
            for (int ry = 0; ry < gl.h; ++ry) {
                for (int rx = 0; rx < gl.w; ++rx) {
                    const uint32_t idx = static_cast<uint32_t>(ry) * gl.w + rx;
                    const uint8_t b = f.data[gl.byte_offset + (idx >> 1)];
                    const uint8_t a = (idx & 1u) ? (b & 0x0Fu) : (b >> 4);
                    if (a > 0) c.BlendPixel4bpp(cx + gl.ox + rx, y + gl.oy + dy + ry, fg, a / 15.0f);
                }
            }
            cx += gl.adv + tracking;
            break;
        }
    }
}

void DrawUrgencyDot4bpp(PixelCanvas& c, float cx, float cy, UrgencyLevel u) {
    if (u == UrgencyLevel::kCritical) {
        c.DrawAACircle4bpp(cx, cy, 3.6f, 1);
    } else if (u == UrgencyLevel::kSoon) {
        c.DrawAAArc4bpp(cx, cy, 3.3f, 1.5f, 0.0f, 360.0f, 4);
    } else {
        c.DrawAACircle4bpp(cx, cy, 2.2f, 11);
    }
}

void DrawUrgencyDot1bpp(PixelCanvas& c, int cx, int cy, UrgencyLevel u, uint8_t fg) {
    if (u == UrgencyLevel::kCritical) {
        c.Circle(cx, cy, 3, fg, true);
    } else if (u == UrgencyLevel::kSoon) {
        c.Circle(cx, cy, 3, fg, false);
    } else {
        c.Circle(cx, cy, 1, fg, true);
    }
}

struct ParsedDateParts {
    std::string month_day_cn;  // "11月24日" or fallback
    std::string month_cn;      // "11月"
    std::string day_num;       // "24" or "--"
    std::string weekday_full;  // "星期二" or ""
    std::string header_line;   // "2026 · 11月 · 星期二"
};

ParsedDateParts ParseStandbyDate(const std::string& date_str) {
    ParsedDateParts p;
    int m = 0, d = 0;
    const size_t slash = date_str.find('/');
    if (slash != std::string::npos && slash > 0) {
        m = std::atoi(date_str.c_str());
        d = std::atoi(date_str.c_str() + slash + 1);
    }
    const size_t sp = date_str.find(' ');
    std::string wd = (sp != std::string::npos) ? date_str.substr(sp + 1) : "";
    if (wd.rfind("周", 0) == 0 && wd.size() > 3) {
        p.weekday_full = "星期" + wd.substr(3);
    } else {
        p.weekday_full = wd;
    }

    if (m >= 1 && m <= 12 && d >= 1 && d <= 31) {
        p.month_cn = std::to_string(m) + "月";
        p.day_num = std::to_string(d);
        p.month_day_cn = p.month_cn + p.day_num + "日";
    } else {
        p.month_cn = "";
        p.day_num = "--";
        p.month_day_cn = date_str.empty() ? "极趣小冰箱" : date_str;
    }

    std::string yr;
    const time_t now = std::time(nullptr);
    struct tm lt = {};
    localtime_r(&now, &lt);
    if (lt.tm_year + 1900 >= 2025) yr = std::to_string(lt.tm_year + 1900);

    if (!p.month_cn.empty()) {
        if (!yr.empty()) p.header_line = yr + " · " + p.month_cn;
        else p.header_line = p.month_cn;
        if (!p.weekday_full.empty()) p.header_line += " · " + p.weekday_full;
    } else {
        p.header_line = p.month_day_cn;
    }
    return p;
}

std::string SafeHm(const std::string& hm) {
    if (hm.size() >= 5 && hm[2] == ':') return hm.substr(0, 5);
    return "08:00";
}

}  // namespace

FridgeUi::FridgeUi(zectrix_epd_handle_t epd) : epd_(epd) {}

esp_err_t FridgeUi::Flush1bpp(bool force_full) {
    if (!epd_) return ESP_ERR_INVALID_STATE;
    const bool use_full = force_full || !has_1bpp_base_ ||
                          partial_count_ >= kMaxPartialBeforeFull;
    esp_err_t err = zectrix_epd_power_on(epd_);
    if (err != ESP_OK) return err;
    if (use_full) {
        err = zectrix_epd_refresh_full_1bpp(epd_, canvas_.data1bpp(), canvas_.size1bpp());
        if (err == ESP_OK) { has_1bpp_base_ = true; partial_count_ = 0; pill_partial_count_ = 0; }
    } else {
        const zectrix_epd_rect_t full = {0, 0, PixelCanvas::kWidth, PixelCanvas::kHeight};
        err = zectrix_epd_refresh_partial_1bpp(epd_, &full, canvas_.data1bpp(), canvas_.size1bpp());
        if (err == ESP_OK) {
            partial_count_++;
        } else {
            has_1bpp_base_ = false;
            err = zectrix_epd_refresh_full_1bpp(epd_, canvas_.data1bpp(), canvas_.size1bpp());
            if (err == ESP_OK) { has_1bpp_base_ = true; partial_count_ = 0; pill_partial_count_ = 0; }
        }
    }
    zectrix_epd_power_off(epd_);
    return err;
}

esp_err_t FridgeUi::FlushRect1bpp(const zectrix_epd_rect_t& rect, bool count_for_full) {
    if (!epd_) return ESP_ERR_INVALID_STATE;
    if (!has_1bpp_base_ || (count_for_full && partial_count_ >= kMaxPartialBeforeFull)) {
        return Flush1bpp(true);
    }
    zectrix_epd_rect_t aligned = {};
    std::vector<uint8_t> patch;
    canvas_.ExtractPatch1bpp(rect, &aligned, &patch);
    esp_err_t err = zectrix_epd_power_on(epd_);
    if (err != ESP_OK) return err;
    err = zectrix_epd_refresh_partial_1bpp(epd_, &aligned, patch.data(), patch.size());
    zectrix_epd_power_off(epd_);
    if (err == ESP_OK) {
        if (count_for_full) partial_count_++;
        return ESP_OK;
    }
    has_1bpp_base_ = false;
    return Flush1bpp(true);
}

esp_err_t FridgeUi::ClearDisplay() {
    canvas_.SetMode4bpp(false);
    canvas_.Clear(15);
    has_1bpp_base_ = false;
    return Flush1bpp(true);
}

// ============================================================================
// Shared building blocks
// ============================================================================
int FridgeUi::DrawChip(int x, int y, const std::string& text, bool solid) {
    const int w = canvas_.MeasureText(text, 1) + 12;
    if (solid) {
        canvas_.FillRoundRectR(x, y, w, 19, 9, 0);
        canvas_.DrawText(x + 6, y + 1, text, 1, 15);
    } else {
        canvas_.FillRoundRectR(x, y, w, 19, 9, 15);
        canvas_.RoundRectR(x, y, w, 19, 9, 0, 1);
        canvas_.DrawText(x + 6, y + 1, text, 1, 0);
    }
    return w;
}

int FridgeUi::DrawKeyHints(int x, int y, const std::vector<KeyHint>& hints) {
    for (const auto& h : hints) {
        const std::string key = h.key;
        if (is_minimal()) {
            const int kw = canvas_.MeasureText(key, 1) + 8;
            canvas_.RoundRectR(x, y, kw, 19, 4, 0, 1);
            canvas_.DrawText(x + 4, y + 1, key, 1, 0);
            x += kw + 5;
            canvas_.DrawText(x, y + 1, h.desc, 1, 0);
            x += canvas_.MeasureText(h.desc, 1) + 14;
        } else {
            const int kw = canvas_.MeasureText(key, 1) + 10;
            canvas_.FillRoundRectR(x, y, kw, 20, 5, 0);
            canvas_.DrawText(x + 5, y + 2, key, 1, 15);
            x += kw + 4;
            canvas_.DrawText(x, y + 2, h.desc, 1, 0);
            x += canvas_.MeasureText(h.desc, 1) + 12;
        }
    }
    return x;
}

void FridgeUi::DrawButton(int x, int y, int w, int h, const std::string& label,
                          UiIcon icon, bool has_icon, bool selected) {
    if (is_minimal()) {
        const int r = 8;
        if (selected) {
            canvas_.FillRoundRectR(x, y, w, h, r, 0);
        } else {
            canvas_.FillRoundRectR(x, y, w, h, r, 15);
            canvas_.RoundRectR(x, y, w, h, r, 0, 1);
        }
        const int max_text = std::max(17, w - 16);
        const int tw = std::min(max_text, canvas_.MeasureText(label, 1));
        const int tx = x + (w - tw) / 2;
        const int ty = y + (h - 17) / 2;
        canvas_.DrawText(tx, ty, label, 1, selected ? 15 : 0, max_text);
        return;
    }

    const int r = std::min(10, h / 2);
    const int icon_w = has_icon ? 20 : 0;
    const int tw = canvas_.MeasureText(label, 1);
    const int pointer_w = selected ? 12 : 0;
    const int content_w = pointer_w + icon_w + tw;
    const int max_text = w - 16 - pointer_w - icon_w;
    int cx = x + std::max(8, (w - content_w) / 2);
    const int ty = y + (h - 17) / 2;
    if (selected) {
        canvas_.FillRoundRectR(x, y, w, h, r, 0);
        canvas_.DrawPointer(cx, y + (h - 9) / 2, 15);
        cx += pointer_w;
    } else {
        // "keycap" look: outline + thicker bottom lip
        canvas_.FillRoundRectR(x, y, w, h, r, 15);
        canvas_.RoundRectR(x, y, w, h, r, 0, 1);
        canvas_.HLine(x + r, y + h - 2, w - 2 * r, 0);
    }
    const uint8_t fg = selected ? 15 : 0;
    if (has_icon) {
        canvas_.DrawIcon16(icon, cx, y + (h - 16) / 2, fg);
        cx += icon_w;
    }
    canvas_.DrawText(cx, ty, label, 1, fg, std::max(17, max_text));
}

void FridgeUi::DrawPopupFrame(int px, int py, int pw, int ph, UiIcon icon,
                              const std::string& title, Mood mood) {
    canvas_.SetMode4bpp(false);
    // Clear footprint (incl. shadow) so the page underneath never bleeds through
    canvas_.FillRect(px - 6, py - 6, pw + 14, ph + 14, 15);
    if (is_minimal()) {
        // Crisp pure-white gallery modal: zero checkerboard shadow, zero dot band, zero mascot
        canvas_.FillRoundRectR(px, py, pw, ph, 10, 15);
        canvas_.RoundRectR(px, py, pw, ph, 10, 0, 2);
        canvas_.DrawText(px + 18, py + 12, title, 1, 0, pw - 36);
        canvas_.HLine(px + 16, py + 37, pw - 32, 0);
        return;
    }

    canvas_.FillPatternRoundRectR(px + 4, py + 4, pw, ph, 10, UiFill::kChecker50);
    canvas_.FillRoundRectR(px, py, pw, ph, 10, 15);
    // Soft dot-tint header band (clean in 1bpp without checkerboard clutter)
    canvas_.FillPatternRoundRectR(px + 2, py + 2, pw - 4, 44, 8, UiFill::kDots25);
    canvas_.FillRect(px + 2, py + 38, pw - 4, 8, 15);
    canvas_.HLine(px + 2, py + 37, pw - 4, 0);
    canvas_.RoundRectR(px, py, pw, ph, 10, 0, 2);

    // White title tag on the band
    const int mascot_w = (mood == Mood::kNone) ? 0 : 44;
    const int tag_max = pw - 24 - mascot_w;
    const int text_w = std::min(canvas_.MeasureText(title, 1), tag_max - 34);
    const int tag_w = text_w + 34;
    canvas_.FillRoundRectR(px + 10, py + 7, tag_w, 25, 12, 15);
    canvas_.RoundRectR(px + 10, py + 7, tag_w, 25, 12, 0, 1);
    canvas_.DrawIcon16(icon, px + 17, py + 11, 0);
    canvas_.DrawText(px + 37, py + 11, title, 1, 0, text_w);

    if (mood != Mood::kNone) {
        // Little fridge friend peeking over the band edge
        canvas_.DrawMascotPixel(px + pw - 50, py + 4, 2, mood == Mood::kWorried);
    }
}

void FridgeUi::DrawStatusArea(const UiTopBar& bar) {
    canvas_.FillRect(kStatusX, 0, kStatusW, kStatusH, 15);
    if (is_minimal()) {
        if (!bar.active_timer_label.empty()) {
            const int tw = canvas_.MeasureText(bar.active_timer_label, 1);
            const int lw = tw + 16;
            const int lx = std::max(kStatusX, 360 - lw);
            canvas_.FillRoundRectR(lx, 5, lw, 20, 10, 0);
            canvas_.DrawText(lx + 8, 7, bar.active_timer_label, 1, 15, lw - 12);
        } else if (bar.ap_mode) {
            const std::string ap = "AP配网";
            canvas_.DrawText(360 - canvas_.MeasureText(ap, 1), 6, ap, 1, 0);
        } else {
            canvas_.DrawText(360 - canvas_.MeasureText(bar.hm_time, 1), 6, bar.hm_time, 1, 0);
        }
        canvas_.DrawMinimalBattery(366, 9, bar.battery_pct, bar.charging, 0);
        return;
    }

    if (!bar.active_timer_label.empty()) {
        const int tw = canvas_.MeasureText(bar.active_timer_label, 1);
        const int lw = tw + 26;
        const int lx = std::max(kStatusX, 366 - lw);
        canvas_.FillRoundRectR(lx, 4, lw, 20, 10, 0);
        canvas_.DrawClockIcon(lx + 6, 9, 15);
        canvas_.DrawText(lx + 19, 6, bar.active_timer_label, 1, 15, lw - 22);
    } else if (bar.ap_mode) {
        canvas_.DrawText(306, 6, "AP配网", 1, 0);
    } else {
        canvas_.DrawWifiIcon(304, 8, bar.wifi_ok, 0);
        canvas_.DrawText(318, 6, bar.hm_time, 1, 0);
    }
    canvas_.DrawBatteryIcon(371, 8, bar.battery_pct, bar.charging, 0, 15);
}

void FridgeUi::DrawTopNav(FridgeTab active, const UiTopBar& bar) {
    static constexpr const char* kNames[] = {"库存", "食谱", "烹饪", "采购", "设置"};
    canvas_.FillRect(0, 0, PixelCanvas::kWidth, 31, 15);
    nav_focus_ = bar.nav_focus;

    if (is_minimal()) {
        canvas_.HLine(12, 30, 376, 0);
        int x = 12;
        for (int i = 0; i < static_cast<int>(FridgeTab::kCount); ++i) {
            const std::string t = kNames[i];
            const int tw = canvas_.MeasureText(t, 1);
            if (i == static_cast<int>(active)) {
                if (bar.nav_focus) {
                    canvas_.FillRoundRectR(x - 6, 4, tw + 12, 22, 11, 0);
                    canvas_.DrawText(x, 6, t, 1, 15);
                } else {
                    canvas_.DrawText(x, 6, t, 1, 0);
                    canvas_.FillRect(x - 1, 27, tw + 2, 3, 0);
                }
            } else {
                canvas_.DrawText(x, 6, t, 1, 0);
            }
            x += tw + 18;
        }
        DrawStatusArea(bar);
        return;
    }

    canvas_.FillRect(0, 28, PixelCanvas::kWidth, 2, 0);
    int x = 4;
    for (int i = 0; i < static_cast<int>(FridgeTab::kCount); ++i) {
        const std::string t = kNames[i];
        const int tw = canvas_.MeasureText(t, 1);
        const int tab_w = tw + 16;
        if (i == static_cast<int>(active)) {
            if (bar.nav_focus) {
                canvas_.FillRoundRectR(x, 2, tab_w, 24, 7, 0);
                canvas_.FillRect(x, 16, tab_w, 12, 0);
                canvas_.DrawText(x + 8, 6, t, 1, 15);
            } else {
                canvas_.FillRoundRectR(x, 2, tab_w, 26, 7, 0);
                canvas_.FillRect(x, 18, tab_w, 12, 0);
                canvas_.FillRoundRectR(x + 2, 4, tab_w - 4, 24, 5, 15);
                canvas_.FillRect(x + 2, 18, tab_w - 4, 12, 15);
                canvas_.DrawText(x + 8, 6, t, 1, 0);
            }
        } else {
            canvas_.DrawText(x + 8, 6, t, 1, 0);
        }
        x += tab_w + 2;
    }
    DrawStatusArea(bar);
}

// Clean bottom key-legend bar
void FridgeUi::DrawKeyLegend(const std::string& nav_label, const std::string& ok_label) {
    if (is_minimal()) {
        canvas_.FillRect(0, 270, PixelCanvas::kWidth, 30, 15);
        canvas_.HLine(12, 271, 376, 0);
        if (nav_focus_) {
            DrawKeyHints(12, 276, {{"上/下", "切页"}, {"OK", "进入本页"}});
            const std::string hint = "按住OK说话";
            canvas_.DrawText(388 - canvas_.MeasureText(hint, 1), 277, hint, 1, 0);
            return;
        }
        DrawKeyHints(12, 276, {{"上/下", nav_label}, {"OK", ok_label}});
        const std::string hold = "长按上", act = "菜单";
        const int kw = canvas_.MeasureText(hold, 1) + 8;
        const int rx = 388 - (kw + 5 + canvas_.MeasureText(act, 1));
        canvas_.RoundRectR(rx, 276, kw, 19, 4, 0, 1);
        canvas_.DrawText(rx + 4, 277, hold, 1, 0);
        canvas_.DrawText(rx + kw + 5, 277, act, 1, 0);
        return;
    }

    canvas_.FillRect(0, kLegendY, PixelCanvas::kWidth, PixelCanvas::kHeight - kLegendY, 15);
    canvas_.DottedHLine(8, kLegendY + 1, PixelCanvas::kWidth - 16, 4, 0);
    canvas_.HLine(0, kLegendY + 5, PixelCanvas::kWidth, 0);

    if (nav_focus_) {
        DrawKeyHints(6, 276, {{"上/下", "切换页面"}, {"OK", "进入本页"}});
        const std::string hint = "按住OK说话";
        canvas_.DrawText(394 - canvas_.MeasureText(hint, 1), 278, hint, 1, 0);
        return;
    }
    DrawKeyHints(6, 276, {{"上/下", nav_label}, {"OK", ok_label}});

    const std::string hold = "长按上";
    const std::string act = "菜单";
    const int kw = canvas_.MeasureText(hold, 1) + 10;
    const int total = kw + 4 + canvas_.MeasureText(act, 1);
    const int x = 394 - total;
    canvas_.RoundRectR(x, 276, kw, 20, 5, 0, 1);
    canvas_.DrawText(x + 5, 278, hold, 1, 0);
    canvas_.DrawText(x + kw + 4, 278, act, 1, 0);
}

esp_err_t FridgeUi::UpdateTimerPillOnly(const UiTopBar& bar) {
    canvas_.SetMode4bpp(false);
    DrawStatusArea(bar);
    // Periodically clear ghosting on very long stews (every 30 pill ticks ~30 min)
    if (++pill_partial_count_ >= kMaxPillPartialBeforeFull) {
        return Flush1bpp(true);
    }
    return FlushRect1bpp({kStatusX, 0, kStatusW, kStatusH}, false);
}

// ============================================================================
// Boot splash (Minimalist Gallery vs Warm Card Stationery)
// ============================================================================
esp_err_t FridgeUi::ShowBootSplash(const std::string& subtitle) {
    canvas_.SetMode4bpp(false);
    canvas_.Clear(15);
    if (is_minimal()) {
        canvas_.DrawTextCentered(86, "极趣小冰箱", 2, 0);
        canvas_.HLine(182, 128, 36, 0);
        const int sw = std::min(340, canvas_.MeasureText(subtitle, 1) + 28);
        const int sx = (PixelCanvas::kWidth - sw) / 2;
        canvas_.RoundRectR(sx, 146, sw, 28, 14, 0, 1);
        canvas_.DrawText(sx + 14, 151, subtitle, 1, 0, sw - 24);
        canvas_.DrawTextCentered(198, "上/下 选菜 · OK 确认 · 按住 OK 说话", 1, 0);
        canvas_.DrawTextCentered(256, "ZECTRIX NOTE4", 1, 0);
        return Flush1bpp(true);
    }

    canvas_.FillPattern(0, 0, PixelCanvas::kWidth, 14, UiFill::kGingham);
    canvas_.HLine(0, 14, PixelCanvas::kWidth, 0);
    canvas_.FillPattern(0, 286, PixelCanvas::kWidth, 14, UiFill::kGingham);
    canvas_.HLine(0, 285, PixelCanvas::kWidth, 0);

    canvas_.DrawMascotPixel(170, 30, 3, false);
    canvas_.DrawIcon16(UiIcon::kSpark, 238, 34, 0);
    canvas_.DrawIcon16(UiIcon::kHeart, 146, 52, 0);
    canvas_.DrawTextCentered(98, "极趣小冰箱", 2, 0);

    const int sw = canvas_.MeasureText(subtitle, 1) + 24;
    const int sx = (PixelCanvas::kWidth - sw) / 2;
    canvas_.RoundRectR(sx, 140, sw, 26, 13, 0, 1);
    canvas_.DrawText(sx + 12, 144, subtitle, 1, 0, sw - 20);

    canvas_.DrawTextCentered(186, "上下键选择 · OK进入 · 按住OK说话", 1, 0);
    canvas_.DottedHLine(120, 216, 160, 3, 0);
    canvas_.DrawTextCentered(228, "把每一样食材都好好吃掉~", 1, 0);
    canvas_.DrawTextCentered(256, "ZECTRIX NOTE4", 1, 0);
    return Flush1bpp(true);
}

// ============================================================================
// Page 0: True 4bpp (16-gray) Standby Poster — 5 Curated Themes:
//   0 = MUJI 瑞士双栏 (Minimalist Swiss Split)
//   1 = Apple 极简大时钟桌搭 (StandBy Clock + Soft Stone Blocks)
//   2 = Kindle 单向历书页日历 (Editorial Book Calendar + AA Freshness Ring)
//   3 = 原研哉 黑白撞色海报 (High-Contrast Monolith)
//   4 = 温馨圆角卡片看板 (Unified 16-Gray Card Theme Companion)
// ============================================================================
esp_err_t FridgeUi::RenderStandby4bpp(const std::string& date_str,
                                      const std::string& hm_str,
                                      int battery_pct, bool charging,
                                      const std::vector<FridgeItem>& all_items,
                                      const RecipeMatchView* top_recipe,
                                      const std::vector<ShoppingItem>& shopping,
                                      int wake_hour,
                                      int standby_style) {
    if (!epd_) return ESP_ERR_INVALID_STATE;
    canvas_.SetMode4bpp(false);
    canvas_.Clear(15);
    Flush1bpp(true);

    canvas_.SetMode4bpp(true);
    canvas_.Clear(15);

    const int32_t today = FridgeStore::TodayDayNumber();
    int exp_count = 0;
    for (const auto& it : all_items) {
        bool has_exp = false;
        if (FridgeStore::ComputeItemDaysLeft(it, today, &has_exp) <= 2 && has_exp) exp_count++;
    }
    int pending = 0;
    std::string shop_a, shop_b;
    for (const auto& sh : shopping) {
        if (sh.checked) continue;
        pending++;
        if (pending == 1) shop_a = sh.name;
        else if (pending == 2) shop_b = sh.name;
    }

    const ParsedDateParts dp = ParseStandbyDate(date_str);
    const std::string hm = SafeHm(hm_str);
    const std::string hh = hm.substr(0, 2);
    const std::string mm = hm.substr(3, 2);
    const std::string bp = std::to_string(battery_pct) + "%";
    const int style = std::clamp(standby_style, 0, 4);

    if (style == 0) {
        // ---- Style 0: MUJI · 瑞士双栏呼吸感海报 ----
        canvas_.FillRect(0, 0, 150, 300, 14);
        canvas_.VLine(150, 0, 300, 13);

        const int w_hh = MeasureAa(standby_aa::kFontHelvThin68, hh, -2);
        const int w_mm = MeasureAa(standby_aa::kFontHelvThin68, mm, -2);
        DrawAaText(canvas_, standby_aa::kFontHelvThin68, (150 - w_hh) / 2, 26, hh, 1, -2);
        DrawAaText(canvas_, standby_aa::kFontHelvThin68, (150 - w_mm) / 2, 92, mm, 4, -2);

        canvas_.HLine(59, 174, 32, 10);
        const int d1w = std::min(136, canvas_.MeasureText(dp.month_day_cn, 1));
        canvas_.DrawText((150 - d1w) / 2, 188, dp.month_day_cn, 1, 2, 136);
        if (!dp.weekday_full.empty()) {
            canvas_.DrawText((150 - canvas_.MeasureText(dp.weekday_full, 1)) / 2, 210, dp.weekday_full, 1, 6);
        }

        canvas_.DrawMinimalBattery(42, 266, battery_pct, charging, 6);
        canvas_.DrawText(70, 263, bp, 1, 6);

        // Right editorial column
        canvas_.DrawText(172, 20, "优先食用", 1, 7);
        std::string meta = "共" + std::to_string(all_items.size()) + "样";
        if (exp_count > 0) meta += " · " + std::to_string(exp_count) + "临期";
        canvas_.DrawText(380 - canvas_.MeasureText(meta, 1), 20, meta, 1, 6);
        canvas_.HLine(172, 44, 208, 13);

        if (all_items.empty()) {
            canvas_.DrawText(172, 114, "冰箱空空的 · 按 OK 唤醒记录", 1, 7, 208);
        } else {
            const int show = std::min<int>(4, static_cast<int>(all_items.size()));
            for (int i = 0; i < show; ++i) {
                const auto& it = all_items[i];
                const int y = 56 + i * 36;
                if (i > 0) canvas_.HLine(172, y - 6, 208, 14);
                UrgencyLevel urg = UrgencyLevel::kNone;
                const std::string tag = FormatItemDateTag(it, today, &urg);
                DrawUrgencyDot4bpp(canvas_, 178.0f, y + 9.0f, urg);
                canvas_.DrawText(190, y, it.name, 1, urg == UrgencyLevel::kNone ? 3 : 0, 82);
                canvas_.DrawText(272, y, it.qty, 1, 8, 38);
                const int tw = canvas_.MeasureText(tag, 1);
                if (urg == UrgencyLevel::kCritical) {
                    canvas_.DrawAARoundRect4bpp(380 - tw - 12, y - 2, tw + 12, 21, 10.5f, 13);
                    canvas_.DrawText(380 - tw - 6, y, tag, 1, 0);
                } else {
                    canvas_.DrawText(380 - tw, y, tag, 1, urg == UrgencyLevel::kSoon ? 3 : 8);
                }
            }
        }

        canvas_.HLine(172, 204, 208, 12);
        canvas_.DrawText(172, 214, "今晚推荐", 1, 7);
        if (top_recipe != nullptr) {
            canvas_.DrawText(172, 234, top_recipe->recipe.title, 2, 1, 126);
            canvas_.DrawAARoundRect4bpp(172, 273, 96, 3, 1.5f, 13);
            const float fw = std::max(8.0f, 96.0f * std::clamp(top_recipe->match_pct, 0, 100) / 100.0f);
            canvas_.DrawAARoundRect4bpp(172, 273, fw, 3, 1.5f, 3);
        } else {
            canvas_.DrawText(172, 240, "暂无食谱", 1, 7);
        }

        canvas_.VLine(306, 216, 62, 13);
        canvas_.DrawText(318, 214, "待买", 1, 7);
        const std::string pnum = std::to_string(pending);
        DrawAaText(canvas_, standby_aa::kFontHelvLight30, 318, 236, pnum, 1);
        canvas_.DrawText(318 + MeasureAa(standby_aa::kFontHelvLight30, pnum) + 5, 245, "样", 1, 6);
        canvas_.DrawText(318, 267,
                         shop_a.empty() ? "已清空" : (shop_a + (pending > 1 ? "等" : "")),
                         1, 7, 64);
    } else if (style == 1) {
        // ---- Style 1: Apple StandBy · 极简大时钟桌搭 ----
        const std::string top_d = dp.weekday_full.empty()
                                      ? dp.month_day_cn
                                      : (dp.month_day_cn + " " + dp.weekday_full);
        canvas_.DrawText(24, 16, top_d, 1, 6, 240);
        canvas_.DrawText(344 - canvas_.MeasureText(bp, 1), 16, bp, 1, 6);
        canvas_.DrawMinimalBattery(352, 19, battery_pct, charging, 5);

        DrawAaText(canvas_, standby_aa::kFontHelvLight82, 22, 46, hm, 1, -1, -6);

        const float ccx = 338.0f, ccy = 88.0f;
        canvas_.DrawAACircle4bpp(ccx, ccy, 36.0f, 14);
        canvas_.DrawAAArc4bpp(ccx, ccy, 33.0f, 3.0f, 0.0f, 360.0f, 13);
        if (exp_count > 0 && !all_items.empty()) {
            const float ratio = std::min(1.0f, static_cast<float>(exp_count) /
                                                   static_cast<float>(all_items.size()));
            canvas_.DrawAAArc4bpp(ccx, ccy, 33.0f, 3.0f, 0.0f, 360.0f * std::max(0.15f, ratio), 2);
        }
        const std::string ec = std::to_string(exp_count);
        DrawAaText(canvas_, standby_aa::kFontHelvLight30,
                   static_cast<int>(ccx) - MeasureAa(standby_aa::kFontHelvLight30, ec) / 2, 66, ec, 0);
        const std::string ecl = (exp_count > 0) ? "临期" : "新鲜";
        canvas_.DrawText(static_cast<int>(ccx) - canvas_.MeasureText(ecl, 1) / 2, 95, ecl, 1, 6);

        canvas_.DrawAARoundRect4bpp(20, 144, 222, 134, 12, 14);
        canvas_.DrawAARoundRect4bpp(252, 144, 128, 134, 12, 14);

        canvas_.DrawText(34, 154, "优先食用", 1, 6);
        const std::string tot = "共" + std::to_string(all_items.size()) + "样";
        canvas_.DrawText(228 - canvas_.MeasureText(tot, 1), 154, tot, 1, 7);
        if (all_items.empty()) {
            canvas_.DrawText(34, 206, "冰箱空空的~", 1, 7);
        } else {
            const int show = std::min<int>(3, static_cast<int>(all_items.size()));
            for (int i = 0; i < show; ++i) {
                const auto& it = all_items[i];
                const int y = 182 + i * 31;
                if (i > 0) canvas_.HLine(34, y - 6, 194, 13);
                UrgencyLevel urg = UrgencyLevel::kNone;
                const std::string tag = FormatItemDateTag(it, today, &urg);
                DrawUrgencyDot4bpp(canvas_, 39.0f, y + 9.0f, urg);
                canvas_.DrawText(50, y, it.name, 1, urg == UrgencyLevel::kNone ? 3 : 0, 88);
                const int tw = canvas_.MeasureText(tag, 1);
                if (urg == UrgencyLevel::kCritical) {
                    canvas_.DrawAARoundRect4bpp(230 - tw - 10, y - 2, tw + 10, 21, 10.5f, 15);
                    canvas_.DrawText(230 - tw - 5, y, tag, 1, 0);
                } else {
                    canvas_.DrawText(230 - tw, y, tag, 1, 6);
                }
            }
        }

        canvas_.DrawText(264, 154, "今晚推荐", 1, 6);
        if (top_recipe != nullptr) {
            canvas_.DrawText(264, 176, top_recipe->recipe.title, 2, 1, 108);
        } else {
            canvas_.DrawText(264, 182, "暂无食谱", 1, 7);
        }
        canvas_.HLine(264, 218, 104, 13);
        canvas_.DrawText(264, 227, "待买 " + std::to_string(pending) + " 样", 1, 3);
        canvas_.DrawText(264, 249,
                         shop_a.empty() ? "已清空" : (shop_a + (pending > 1 ? "等" : "")),
                         1, 6, 106);
    } else if (style == 2) {
        // ---- Style 2: Kindle · 单向历书页日历封面 ----
        const int fresh_pct = all_items.empty()
                                  ? 100
                                  : (100 * (static_cast<int>(all_items.size()) - exp_count)) /
                                        static_cast<int>(all_items.size());

        canvas_.DrawText(24, 16, dp.header_line, 1, 6, 232);
        canvas_.DrawText(316 - canvas_.MeasureText(hm, 1), 16, hm, 1, 3);
        canvas_.DrawMinimalBattery(326, 19, battery_pct, charging, 6);
        canvas_.DrawText(354, 16, bp, 1, 7);
        canvas_.HLine(24, 40, 352, 12);

        if (dp.day_num != "--") {
            DrawAaText(canvas_, standby_aa::kFontNewYork82, 22, 58, dp.day_num, 1, -2);
        } else {
            DrawAaText(canvas_, standby_aa::kFontHelvThin68, 22, 62, hh, 1, -2);
        }

        canvas_.DrawText(132, 60, "今晚推荐", 1, 7);
        if (top_recipe != nullptr) {
            canvas_.DrawText(132, 81, top_recipe->recipe.title, 2, 1, 164);
        } else {
            canvas_.DrawText(132, 86, "暂无食谱", 1, 7);
        }
        std::string sub = "共" + std::to_string(all_items.size()) + "样";
        if (exp_count > 0) sub += " · " + std::to_string(exp_count) + "样优先吃";
        else sub += " · 全部新鲜";
        canvas_.DrawText(132, 118, sub, 1, 6, 164);

        const float rcx = 344.0f, rcy = 94.0f;
        canvas_.DrawAAArc4bpp(rcx, rcy, 29.0f, 3.4f, 0.0f, 360.0f, 13);
        if (fresh_pct > 0) {
            canvas_.DrawAAArc4bpp(rcx, rcy, 29.0f, 3.4f, 0.0f, 360.0f * fresh_pct / 100.0f, 2);
        }
        const std::string fp = std::to_string(fresh_pct) + "%";
        DrawAaText(canvas_, standby_aa::kFontHelvMed18,
                   static_cast<int>(rcx) - MeasureAa(standby_aa::kFontHelvMed18, fp) / 2, 85, fp, 1);
        const std::string fl = "新鲜度";
        canvas_.DrawText(static_cast<int>(rcx) - canvas_.MeasureText(fl, 1) / 2, 129, fl, 1, 7);

        canvas_.HLine(24, 154, 352, 13);
        canvas_.DrawText(24, 163, "优先食用", 1, 7);
        if (all_items.empty()) {
            canvas_.DrawText(24, 206, "暂无食材记录", 1, 8);
        } else {
            const int show = std::min<int>(3, static_cast<int>(all_items.size()));
            for (int i = 0; i < show; ++i) {
                const auto& it = all_items[i];
                const int y = 186 + i * 28;
                UrgencyLevel urg = UrgencyLevel::kNone;
                const std::string tag = FormatItemDateTag(it, today, &urg);
                DrawUrgencyDot4bpp(canvas_, 29.0f, y + 9.0f, urg);
                canvas_.DrawText(40, y, it.name, 1, urg == UrgencyLevel::kNone ? 3 : 0, 86);
                const int tw = canvas_.MeasureText(tag, 1);
                canvas_.DrawText(220 - tw, y, tag, 1, urg == UrgencyLevel::kCritical ? 0 : 6);
            }
        }

        canvas_.VLine(238, 164, 102, 13);
        canvas_.DrawText(254, 163, "待买清单 · " + std::to_string(pending), 1, 7);
        int srow = 0;
        for (const auto& s : shopping) {
            if (s.checked) continue;
            const int y = 186 + srow * 28;
            canvas_.DrawAACircle4bpp(259.0f, y + 9.0f, 1.8f, 9);
            canvas_.DrawText(268, y, s.name, 1, 3, 74);
            canvas_.DrawText(376 - canvas_.MeasureText(s.qty, 1), y, s.qty, 1, 8);
            if (++srow >= 3) break;
        }
        if (srow == 0) canvas_.DrawText(254, 206, "清单已清空", 1, 8);

        char foot[64];
        std::snprintf(foot, sizeof(foot), "按 OK 唤醒 · 明早 %02d:00 刷新", wake_hour);
        canvas_.DrawTextCentered(276, foot, 1, 10);
    } else if (style == 3) {
        // ---- Style 3: 原研哉 · 高级黑白撞色美术馆海报 ----
        canvas_.FillRect(0, 0, 400, 104, 1);
        DrawAaText(canvas_, standby_aa::kFontHelvThin68, 22, 19, hm, 15, -1, -5);

        canvas_.DrawText(376 - canvas_.MeasureText(dp.month_day_cn, 1), 22, dp.month_day_cn, 1, 14);
        if (!dp.weekday_full.empty()) {
            canvas_.DrawText(376 - canvas_.MeasureText(dp.weekday_full, 1), 44, dp.weekday_full, 1, 10);
        }
        canvas_.DrawMinimalBattery(316, 72, battery_pct, charging, 12);
        canvas_.DrawText(344, 69, bp, 1, 11);

        auto draw_kpi = [&](int x, const std::string& num, const std::string& label, uint8_t ng) {
            DrawAaText(canvas_, standby_aa::kFontHelvLight30, x, 116, num, ng);
            canvas_.DrawText(x + MeasureAa(standby_aa::kFontHelvLight30, num) + 6, 124, label, 1, 7);
        };
        draw_kpi(24, std::to_string(all_items.size()), "库存", 1);
        draw_kpi(150, std::to_string(exp_count), "临期优先", exp_count > 0 ? 0 : 4);
        draw_kpi(288, std::to_string(pending), "待买", 3);
        canvas_.HLine(24, 152, 352, 13);

        if (all_items.empty()) {
            canvas_.DrawText(24, 204, "暂无食材记录", 1, 7);
        } else {
            const int show = std::min<int>(3, static_cast<int>(all_items.size()));
            for (int i = 0; i < show; ++i) {
                const auto& it = all_items[i];
                const int y = 166 + i * 36;
                if (i > 0) canvas_.HLine(24, y - 8, 196, 14);
                UrgencyLevel urg = UrgencyLevel::kNone;
                const std::string tag = FormatItemDateTag(it, today, &urg);
                DrawUrgencyDot4bpp(canvas_, 29.0f, y + 9.0f, urg);
                canvas_.DrawText(40, y, it.name, 1, urg == UrgencyLevel::kNone ? 3 : 0, 82);
                canvas_.DrawText(122, y, it.qty, 1, 8, 36);
                const int tw = canvas_.MeasureText(tag, 1);
                canvas_.DrawText(220 - tw, y, tag, 1, urg == UrgencyLevel::kCritical ? 0 : 6);
            }
        }

        canvas_.VLine(238, 164, 108, 13);
        canvas_.DrawText(252, 166, "今晚推荐", 1, 7);
        if (top_recipe != nullptr) {
            canvas_.DrawText(252, 190, top_recipe->recipe.title, 2, 1, 128);
            const std::string m = "匹配" + std::to_string(top_recipe->match_pct) + "% · " +
                                  std::to_string(top_recipe->recipe.time_min) + "分";
            canvas_.DrawText(252, 228, m, 1, 6, 128);
            canvas_.DrawAARoundRect4bpp(252, 254, 122, 4, 2, 13);
            const float fw = std::max(8.0f, 122.0f * std::clamp(top_recipe->match_pct, 0, 100) / 100.0f);
            canvas_.DrawAARoundRect4bpp(252, 254, fw, 4, 2, 1);
        } else {
            canvas_.DrawText(252, 196, "暂无食谱", 1, 7);
        }
        canvas_.DrawTextCentered(279, "按 OK 唤醒", 1, 11);
    } else {
        // ---- Style 4: 温馨圆角卡片看板（精修统一版，与圆角卡片内页浑然一体）----
        canvas_.Clear(14);
        auto card = [&](int x, int y, int w, int h) {
            canvas_.FillRoundRectR(x + 2, y + 2, w, h, 10, 12);
            canvas_.FillRoundRectR(x, y, w, h, 10, 15);
            canvas_.RoundRectR(x, y, w, h, 10, 9, 1);
        };
        auto card_band = [&](int x, int y, int w, int bh) {
            canvas_.FillRoundRectR(x + 1, y + 1, w - 2, bh + 6, 9, 14);
            canvas_.FillRect(x + 1, y + bh, w - 2, 7, 15);
            canvas_.HLine(x + 1, y + bh, w - 2, 12);
        };

        card(10, 10, 380, 56);
        canvas_.DrawMascotPixel(18, 18, 2, exp_count > 0);
        DrawAaText(canvas_, standby_aa::kFontHelvLight30, 68, 22, hm, 1, 0, -2);
        canvas_.VLine(152, 20, 36, 13);
        canvas_.DrawText(164, 18, date_str, 1, 2, 166);
        const std::string st_sub = exp_count > 0
                                       ? (std::to_string(exp_count) + " 样食材临期优先吃")
                                       : ("共 " + std::to_string(all_items.size()) + " 样 · 全部新鲜");
        canvas_.DrawText(164, 39, st_sub, 1, 6, 166);
        canvas_.DrawMinimalBattery(355, 22, battery_pct, charging, 4);
        canvas_.DrawText(378 - canvas_.MeasureText(bp, 1), 39, bp, 1, 6);

        card(10, 76, 226, 214);
        card_band(10, 76, 226, 28);
        canvas_.DrawIcon16(UiIcon::kFridge, 18, 82, 2);
        canvas_.DrawText(38, 82, "冰箱库存", 1, 1);
        const std::string cnt_s = "共" + std::to_string(all_items.size()) + "样";
        canvas_.DrawText(226 - canvas_.MeasureText(cnt_s, 1), 82, cnt_s, 1, 6);

        if (all_items.empty()) {
            canvas_.DrawText(36, 168, "冰箱空空的，放点食材吧~", 1, 6);
        } else {
            const int show = std::min<int>(5, static_cast<int>(all_items.size()));
            for (int i = 0; i < show; ++i) {
                const auto& it = all_items[i];
                const int y = 110 + i * 35;
                if (i > 0) canvas_.DottedHLine(20, y - 3, 206, 3, 13);
                UrgencyLevel urg = UrgencyLevel::kNone;
                const std::string tag = FormatItemDateTag(it, today, &urg);
                const UiIcon ico = PixelCanvas::IconForFood(food_catalog::GuessIcon(it.name, it.zone));
                canvas_.Circle(30, y + 14, 12, urg == UrgencyLevel::kCritical ? 13 : 14, true);
                canvas_.Circle(30, y + 14, 12, 10, false);
                canvas_.DrawIcon16(ico, 22, y + 6, urg == UrgencyLevel::kNone ? 3 : 1);
                canvas_.DrawText(48, y + 6, it.name, 1, urg == UrgencyLevel::kNone ? 3 : 0, 72);
                canvas_.DrawText(120, y + 6, it.qty, 1, 7, 36);
                const int tw = canvas_.MeasureText(tag, 1);
                if (urg == UrgencyLevel::kCritical) {
                    canvas_.DrawAARoundRect4bpp(228 - tw - 12, y + 4, tw + 12, 20, 10, 2);
                    canvas_.DrawText(228 - tw - 6, y + 5, tag, 1, 15);
                } else if (urg == UrgencyLevel::kSoon) {
                    canvas_.DrawAARoundRect4bpp(228 - tw - 12, y + 4, tw + 12, 20, 10, 13);
                    canvas_.DrawText(228 - tw - 6, y + 5, tag, 1, 1);
                } else {
                    canvas_.DrawText(228 - tw, y + 6, tag, 1, 7);
                }
            }
        }

        card(246, 76, 144, 114);
        card_band(246, 76, 144, 28);
        canvas_.DrawIcon16(UiIcon::kPot, 254, 82, 2);
        canvas_.DrawText(274, 82, "今晚推荐", 1, 1);
        if (top_recipe != nullptr) {
            canvas_.DrawText(256, 112, top_recipe->recipe.title, 2, 1, 126);
            canvas_.DrawText(256, 148,
                             "匹配" + std::to_string(top_recipe->match_pct) + "% · " +
                                 std::to_string(top_recipe->recipe.time_min) + "分",
                             1, 5, 126);
            canvas_.DrawAARoundRect4bpp(256, 172, 122, 5, 2.5f, 13);
            const float fw = std::max(10.0f, 122.0f * std::clamp(top_recipe->match_pct, 0, 100) / 100.0f);
            canvas_.DrawAARoundRect4bpp(256, 172, fw, 5, 2.5f, 2);
        } else {
            canvas_.DrawText(256, 128, "暂无食谱", 1, 6);
        }

        card(246, 200, 144, 90);
        card_band(246, 200, 144, 26);
        canvas_.DrawIcon16(UiIcon::kBasket, 254, 205, 2);
        canvas_.DrawText(274, 205, "待买清单", 1, 1);
        const std::string ps = std::to_string(pending) + "样";
        canvas_.DrawText(380 - canvas_.MeasureText(ps, 1), 205, ps, 1, 4);
        canvas_.DrawText(256, 234, shop_a.empty() ? "清单已清空~" : ("· " + shop_a), 1, 2, 124);
        if (!shop_b.empty()) {
            canvas_.DrawText(256, 257, "· " + shop_b + (pending > 2 ? "等" : ""), 1, 6, 124);
        }
    }

    esp_err_t err = zectrix_epd_power_on(epd_);
    if (err == ESP_OK) {
        err = zectrix_epd_refresh_full_4bpp(epd_, canvas_.data4bpp(), canvas_.size4bpp());
    }
    zectrix_epd_power_off(epd_);
    canvas_.SetMode4bpp(false);
    has_1bpp_base_ = false;
    return err;
}

// ============================================================================
// Page 1: Fridge Inventory (Master-Detail Hero Focus Card + 5 Right Slim Cards)
// ============================================================================
esp_err_t FridgeUi::RenderFridgePage(const UiTopBar& bar,
                                     int cnt_all, int cnt_fr, int cnt_fz,
                                     const std::vector<FridgeItem>& items,
                                     int selected_idx,
                                     bool force_full) {
    canvas_.SetMode4bpp(false);
    canvas_.Clear(15);
    DrawTopNav(FridgeTab::kFridge, bar);

    const int32_t today = FridgeStore::TodayDayNumber();
    constexpr int kPerPage = kFridgePageSize;
    const int n = static_cast<int>(items.size());
    const bool has_sel = selected_idx >= 0;
    const int clamped = n > 0 ? std::clamp(selected_idx, 0, n - 1) : 0;
    const int page = n > 0 ? (clamped / kPerPage) : 0;
    const int total_pages = std::max(1, (n + kPerPage - 1) / kPerPage);
    std::string sum = "共" + std::to_string(cnt_all) + "样 · 冷藏" + std::to_string(cnt_fr);
    if (cnt_fz > 0) sum += " · 冷冻" + std::to_string(cnt_fz);

    if (items.empty()) {
        if (is_minimal()) {
            canvas_.DrawText(12, 36, "共 0 样", 1, 0);
            canvas_.DrawTextCentered(116, "冰箱空空的", 2, 0);
            canvas_.HLine(184, 154, 32, 0);
            canvas_.DrawTextCentered(172, "按住 OK 说“放两盒牛奶”即可记录", 1, 0);
            DrawKeyLegend("刷新", "按住说");
            return Flush1bpp(force_full);
        }
        canvas_.DrawIcon16(UiIcon::kFridge, 8, 34, 0);
        canvas_.DrawText(28, 34, "共0样 · 冷藏0", 1, 0);
        canvas_.DrawMascotPixel(170, 66, 3, false);
        canvas_.DrawIcon16(UiIcon::kSpark, 236, 68, 0);
        canvas_.DrawTextCentered(140, "冰箱空空的，等你来填满~", 1, 0);
        canvas_.RoundRectR(46, 172, 308, 34, 17, 0, 1);
        canvas_.DrawIcon16(UiIcon::kMic, 60, 181, 0);
        canvas_.DrawText(82, 180, "按住 OK 说“放两盒牛奶”试试", 1, 0, 262);
        DrawKeyLegend("刷新", "按住说");
        return Flush1bpp(force_full);
    }

    if (is_minimal()) {
        // ---- Minimalist Suite Page 1: Swiss Editorial Split (ZERO box clutter) ----
        canvas_.DrawText(12, 36, sum, 1, 0, total_pages > 1 ? 236 : 376);
        if (total_pages > 1) {
            const std::string pg = "长按下翻页  " + std::to_string(page + 1) + "/" + std::to_string(total_pages);
            canvas_.DrawText(388 - canvas_.MeasureText(pg, 1), 36, pg, 1, 0);
        }

        // Left Swiss editorial hero column (x = 12..152)
        const auto& cur = items[clamped];
        const int lcx = 82;
        const std::string z_txt = cur.zone.empty() ? "冷藏" : cur.zone;
        canvas_.DrawText(16, 66, z_txt, 1, 0);
        const std::string idx_s = std::to_string(clamped + 1) + "/" + std::to_string(n);
        canvas_.DrawText(146 - canvas_.MeasureText(idx_s, 1), 66, idx_s, 1, 0);
        canvas_.DottedHLine(16, 88, 130, 3, 0);

        const int bw = canvas_.MeasureText(cur.name, 2);
        if (bw <= 132) {
            canvas_.DrawText(lcx - bw / 2, 114, cur.name, 2, 0);
        } else {
            const int sw = std::min(132, canvas_.MeasureText(cur.name, 1));
            canvas_.DrawText(lcx - sw / 2, 120, cur.name, 1, 0, 132);
        }
        canvas_.HLine(lcx - 14, 152, 28, 0);

        std::string qty_s = cur.qty.empty() ? z_txt : ("数量  " + cur.qty);
        if (canvas_.MeasureText(qty_s, 1) > 134 && !cur.qty.empty()) qty_s = cur.qty;
        const int qw = std::min(134, canvas_.MeasureText(qty_s, 1));
        canvas_.DrawText(lcx - qw / 2, 164, qty_s, 1, 0, 134);

        UrgencyLevel cur_u = UrgencyLevel::kNone;
        const std::string cur_tag = FormatItemDateTag(cur, today, &cur_u);
        const int pill_w = std::max(88, std::min(132, canvas_.MeasureText(cur_tag, 1) + 24));
        const int px = lcx - pill_w / 2;
        const int ct_w = std::min(pill_w - 12, canvas_.MeasureText(cur_tag, 1));
        if (cur_u == UrgencyLevel::kCritical) {
            canvas_.FillRoundRectR(px, 200, pill_w, 24, 12, 0);
            canvas_.DrawText(lcx - ct_w / 2, 203, cur_tag, 1, 15, pill_w - 12);
        } else {
            canvas_.RoundRectR(px, 200, pill_w, 24, 12, 0, cur_u == UrgencyLevel::kSoon ? 2 : 1);
            canvas_.DrawText(lcx - ct_w / 2, 203, cur_tag, 1, 0, pill_w - 12);
        }

        // Single crisp Swiss vertical divider
        canvas_.VLine(158, 62, 198, 0);

        // Right 5 borderless editorial rows
        const int start = page * kPerPage;
        const int end = std::min(n, start + kPerPage);
        const int row_right = (total_pages > 1) ? 382 : 388;
        const int row_w = row_right - 168;
        for (int i = start; i < end; ++i) {
            const auto& it = items[i];
            const int local = i - start;
            const int ry = 62 + local * 40;
            const bool sel = has_sel && (i == clamped);
            const bool next_sel = has_sel && (i + 1 == clamped);
            const uint8_t fg = sel ? 15 : 0;

            if (sel) {
                canvas_.FillRoundRectR(168, ry + 1, row_w, 34, 8, 0);
                for (int k = 0; k < 5; ++k) {
                    canvas_.VLine(167 - k, ry + 18 - (4 - k), (4 - k) * 2 + 1, 0);
                }
            } else if (!next_sel && local < kPerPage - 1 && i + 1 < end) {
                canvas_.DottedHLine(174, ry + 39, row_w - 12, 4, 0);
            }

            UrgencyLevel u = UrgencyLevel::kNone;
            const std::string tag = FormatItemDateTag(it, today, &u);
            DrawUrgencyDot1bpp(canvas_, 180, ry + 18, u, fg);

            const int tw = canvas_.MeasureText(tag, 1);
            const int tx = row_right - 8 - tw;
            canvas_.DrawText(192, ry + 10, it.name, 1, fg, std::max(46, tx - 196));
            if (sel && u == UrgencyLevel::kCritical) {
                canvas_.FillRoundRectR(tx - 6, ry + 8, tw + 12, 20, 10, 15);
                canvas_.DrawText(tx, ry + 10, tag, 1, 0);
            } else if (!sel && u == UrgencyLevel::kCritical) {
                canvas_.RoundRectR(tx - 6, ry + 8, tw + 12, 20, 10, 0, 1);
                canvas_.DrawText(tx, ry + 10, tag, 1, 0);
            } else {
                canvas_.DrawText(tx, ry + 10, tag, 1, fg);
            }
        }

        if (total_pages > 1) {
            canvas_.VLine(391, 64, 194, 0);
            const int thumb_h = std::max(28, 194 / total_pages);
            const int thumb_y = 64 + (page * (194 - thumb_h)) / (total_pages - 1);
            canvas_.FillRect(390, thumb_y, 3, thumb_h, 0);
        }

        DrawKeyLegend("选菜", "操作/按住说");
        return Flush1bpp(force_full);
    }

    // ---- Card Theme Page 1: Master-Detail Hero Focus Card + 5 Right Slim Cards ----
    canvas_.DrawIcon16(UiIcon::kFridge, 8, 34, 0);
    canvas_.DrawText(28, 34, sum, 1, 0, total_pages > 1 ? 224 : 362);

    if (total_pages > 1) {
        const std::string pill = std::to_string(page + 1) + "/" + std::to_string(total_pages) + "页";
        const int pw = canvas_.MeasureText(pill, 1) + 12;
        DrawChip(392 - pw, 33, pill, false);
        const std::string hint = "长按下翻页";
        const int hw = canvas_.MeasureText(hint, 1);
        canvas_.DrawText(392 - pw - 6 - hw, 34, hint, 1, 0);
    }
    canvas_.HLine(8, 53, 384, 0);

    // ---- Left Hero Focus Card (150 x 200) ----
    const auto& cur = items[clamped];
    constexpr int hx = 8, hy = 58, hw = 150, hh = 200;
    if (has_sel) {
        canvas_.FillRoundRectR(hx + 3, hy + 3, hw, hh, 10, 0);
    } else {
        canvas_.FillPatternRoundRectR(hx + 2, hy + 2, hw, hh, 10, UiFill::kDots25);
    }
    canvas_.FillRoundRectR(hx, hy, hw, hh, 10, 15);
    canvas_.FillPatternRoundRectR(hx + 2, hy + 2, hw - 4, 32, 8, UiFill::kDots25);
    canvas_.FillRect(hx + 2, hy + 28, hw - 4, 8, 15);
    canvas_.HLine(hx + 2, hy + 28, hw - 4, 0);
    canvas_.RoundRectR(hx, hy, hw, hh, 10, 0, has_sel ? 2 : 1);

    // Hero header: zone pill on left, item index on right
    const std::string z_txt = cur.zone.empty() ? "冷藏" : cur.zone;
    const int zw = canvas_.MeasureText(z_txt, 1) + 12;
    canvas_.FillRoundRectR(hx + 8, hy + 5, zw, 19, 9, 15);
    canvas_.RoundRectR(hx + 8, hy + 5, zw, 19, 9, 0, 1);
    canvas_.DrawText(hx + 14, hy + 6, z_txt, 1, 0);

    const std::string idx_txt = std::to_string(clamped + 1) + "/" + std::to_string(n);
    const int iw = canvas_.MeasureText(idx_txt, 1) + 10;
    canvas_.FillRoundRectR(hx + hw - iw - 8, hy + 5, iw, 19, 9, 15);
    canvas_.DrawText(hx + hw - iw - 3, hy + 6, idx_txt, 1, 0);

    // Double-ring stamp medallion + crisp 32x32 scaled pixel food icon
    const int mcx = hx + hw / 2;
    const int mcy = hy + 70;
    canvas_.Circle(mcx, mcy, 28, 0, false);
    canvas_.Circle(mcx, mcy, 25, 0, false);
    const UiIcon hero_ico = PixelCanvas::IconForFood(food_catalog::GuessIcon(cur.name, cur.zone));
    canvas_.DrawIconScaled(hero_ico, mcx - 16, mcy - 16, 2, 0);

    // 29px large title if fits cleanly, otherwise centered 17px
    const int big_w = canvas_.MeasureText(cur.name, 2);
    if (big_w <= hw - 16) {
        canvas_.DrawText(mcx - big_w / 2, hy + 106, cur.name, 2, 0);
    } else {
        const int sm_w = std::min(hw - 14, canvas_.MeasureText(cur.name, 1));
        canvas_.DrawText(mcx - sm_w / 2, hy + 113, cur.name, 1, 0, hw - 14);
    }

    // Quantity centered
    std::string qty_line = cur.qty.empty() ? z_txt : ("数量 " + cur.qty);
    if (canvas_.MeasureText(qty_line, 1) > hw - 14 && !cur.qty.empty()) {
        qty_line = cur.qty;
    }
    const int qw = std::min(hw - 14, canvas_.MeasureText(qty_line, 1));
    canvas_.DrawText(mcx - qw / 2, hy + 141, qty_line, 1, 0, hw - 14);

    // Prominent bottom urgency capsule
    UrgencyLevel cur_urg = UrgencyLevel::kNone;
    const std::string cur_tag = FormatItemDateTag(cur, today, &cur_urg);
    const int bw = std::max(96, std::min(hw - 20, canvas_.MeasureText(cur_tag, 1) + 26));
    const int bx = mcx - bw / 2;
    const int by = hy + 166;
    const int ct_w = std::min(bw - 12, canvas_.MeasureText(cur_tag, 1));
    if (cur_urg == UrgencyLevel::kCritical) {
        canvas_.FillRoundRectR(bx, by, bw, 24, 12, 0);
        canvas_.DrawText(mcx - ct_w / 2, by + 3, cur_tag, 1, 15, bw - 12);
    } else if (cur_urg == UrgencyLevel::kSoon) {
        canvas_.RoundRectR(bx, by, bw, 24, 12, 0, 2);
        canvas_.DrawText(mcx - ct_w / 2, by + 3, cur_tag, 1, 0, bw - 12);
    } else {
        canvas_.RoundRectR(bx, by, bw, 24, 12, 0, 1);
        canvas_.DrawText(mcx - ct_w / 2, by + 3, cur_tag, 1, 0, bw - 12);
    }

    // ---- Right Column: 5 Tactile Slim Cards ----
    const int start = page * kPerPage;
    const int end = std::min(n, start + kPerPage);
    constexpr int rx = 170;
    const int rw = (total_pages > 1) ? 214 : 222;
    constexpr int rh = 36;

    for (int i = start; i < end; ++i) {
        const auto& it = items[i];
        const int local = i - start;
        const int ry = 58 + local * 41;
        const bool sel = has_sel && (i == clamped);

        if (sel) {
            canvas_.FillRoundRectR(rx, ry, rw, rh, 8, 0);
            // Left connector triangle pointing straight to the Hero Focus Card
            for (int k = 0; k < 6; ++k) {
                canvas_.VLine(rx - 1 - k, ry + 18 - (5 - k), (5 - k) * 2 + 1, 0);
            }
        } else {
            canvas_.FillRoundRectR(rx, ry, rw, rh, 8, 15);
            canvas_.RoundRectR(rx, ry, rw, rh, 8, 0, 1);
            canvas_.HLine(rx + 8, ry + rh - 2, rw - 16, 0);
        }

        const uint8_t fg = sel ? 15 : 0;
        const UiIcon ico = PixelCanvas::IconForFood(food_catalog::GuessIcon(it.name, it.zone));
        canvas_.DrawIcon16(ico, rx + 9, ry + 10, fg);

        UrgencyLevel urg = UrgencyLevel::kNone;
        const std::string tag = FormatItemDateTag(it, today, &urg);
        const int tw = canvas_.MeasureText(tag, 1) + (urg == UrgencyLevel::kNone ? 2 : 10);
        const int tx = rx + rw - tw - 7;
        canvas_.DrawText(rx + 30, ry + 9, it.name, 1, fg, std::max(44, tx - (rx + 30) - 4));

        if (sel) {
            if (urg == UrgencyLevel::kCritical) {
                canvas_.FillRoundRectR(tx, ry + 8, tw, 19, 9, 15);
                canvas_.DrawText(tx + 5, ry + 9, tag, 1, 0);
            } else if (urg == UrgencyLevel::kSoon) {
                canvas_.RoundRectR(tx, ry + 8, tw, 19, 9, 15, 1);
                canvas_.DrawText(tx + 5, ry + 9, tag, 1, 15);
            } else {
                canvas_.DrawText(tx, ry + 9, tag, 1, 15);
            }
        } else {
            if (urg == UrgencyLevel::kCritical) {
                canvas_.FillRoundRectR(tx, ry + 8, tw, 19, 9, 0);
                canvas_.DrawText(tx + 5, ry + 9, tag, 1, 15);
            } else if (urg == UrgencyLevel::kSoon) {
                canvas_.RoundRectR(tx, ry + 8, tw, 19, 9, 0, 1);
                canvas_.DrawText(tx + 5, ry + 9, tag, 1, 0);
            } else {
                canvas_.DrawText(tx, ry + 9, tag, 1, 0);
            }
        }
    }

    // Sleek right-edge page scrollbar (only if > 1 page)
    if (total_pages > 1) {
        canvas_.RoundRectR(391, 58, 5, 200, 2, 0, 1);
        const int track_h = 196;
        const int thumb_h = std::max(28, track_h / total_pages);
        const int thumb_y = 60 + (page * (track_h - thumb_h)) / (total_pages - 1);
        canvas_.FillRoundRectR(392, thumb_y, 3, thumb_h, 1, 0);
    }

    DrawKeyLegend("选菜", "操作/按住说");
    return Flush1bpp(force_full);
}

// Streamlined 3-action Item Popup (UP = close immediately!)
esp_err_t FridgeUi::RenderItemPopup(const FridgeItem& item, int action_idx, bool has_local_recipe) {
    const int px = 44, py = 40, pw = 312, ph = 214;
    const UiIcon ico = PixelCanvas::IconForFood(food_catalog::GuessIcon(item.name, item.zone));
    DrawPopupFrame(px, py, pw, ph, ico, item.name);

    const int32_t today = FridgeStore::TodayDayNumber();
    UrgencyLevel urg = UrgencyLevel::kNone;
    const std::string tag = FormatItemDateTag(item, today, &urg);
    canvas_.DrawText(px + 16, py + 46, item.qty + " · " + item.zone, 1, 0, pw - 120);
    const int tag_w = canvas_.MeasureText(tag, 1) + 12;
    DrawChip(px + pw - 16 - tag_w, py + 45, tag, urg == UrgencyLevel::kCritical);

    const std::string acts[3] = {
        has_local_recipe ? "用它做道菜 (去食谱)" : "用它做道菜 (AI定制)",
        "吃光啦 (从冰箱移出)",
        "吃光并加入采购清单",
    };
    const UiIcon icons[3] = {UiIcon::kPot, UiIcon::kCheck, UiIcon::kBasket};
    for (int i = 0; i < 3; ++i) {
        const int ay = py + 72 + i * 37;
        DrawButton(px + 14, ay, pw - 28, 31, acts[i], icons[i], true, i == action_idx);
    }

    canvas_.DottedHLine(px + 12, py + ph - 33, pw - 24, 3, 0);
    DrawKeyHints(px + 14, py + ph - 26, {{"上", "退出"}, {"OK", "确认"}, {"下", "下一项"}});
    return FlushRect1bpp({px - 6, py - 6, pw + 16, ph + 16});
}

// ============================================================================
// Page 2: Interlocking Recipe (recipe-card layout; 3 clear buttons)
// ============================================================================
esp_err_t FridgeUi::RenderRecipePage(const UiTopBar& bar,
                                     const std::vector<RecipeMatchView>& recipes,
                                     int recipe_idx, int btn_idx,
                                     bool force_full) {
    canvas_.SetMode4bpp(false);
    canvas_.Clear(15);
    DrawTopNav(FridgeTab::kRecipe, bar);

    if (recipes.empty()) {
        if (is_minimal()) {
            canvas_.DrawTextCentered(116, "还没有食谱", 2, 0);
            canvas_.HLine(184, 154, 32, 0);
            canvas_.DrawTextCentered(172, "按住 OK 说“推荐一道快手菜”", 1, 0);
            DrawKeyLegend("刷新", "按住说");
            return Flush1bpp(force_full);
        }
        canvas_.DrawMascotPixel(170, 64, 3, false);
        canvas_.DrawTextCentered(140, "还没有食谱哦~", 1, 0);
        canvas_.RoundRectR(46, 172, 308, 34, 17, 0, 1);
        canvas_.DrawIcon16(UiIcon::kMic, 60, 181, 0);
        canvas_.DrawText(82, 180, "按住 OK 说“推荐一道快手菜”", 1, 0, 262);
        DrawKeyLegend("刷新", "按住说");
        return Flush1bpp(force_full);
    }

    const int r_i = std::clamp(recipe_idx, 0, static_cast<int>(recipes.size()) - 1);
    const auto& mv = recipes[r_i];

    if (is_minimal()) {
        const std::string match_s = "匹配 " + std::to_string(mv.match_pct) + "%";
        const int mw = canvas_.MeasureText(match_s, 1) + 16;
        canvas_.DrawText(14, 40, mv.recipe.title, 2, 0, 386 - mw - 24);
        canvas_.RoundRectR(386 - mw, 44, mw, 22, 11, 0, 1);
        canvas_.DrawText(386 - mw + 8, 46, match_s, 1, 0);

        const std::string counter = std::to_string(r_i + 1) + "/" + std::to_string(recipes.size()) + "道";
        const int cw = canvas_.MeasureText(counter, 1);
        canvas_.DrawText(14, 76, std::to_string(mv.recipe.time_min) + "分钟 · " + mv.recipe.summary,
                         1, 0, 386 - cw - 24);
        canvas_.DrawText(386 - cw, 76, counter, 1, 0);

        canvas_.HLine(14, 101, 372, 0);
        canvas_.DrawText(14, 108, "所需食材", 1, 0);
        canvas_.DrawText(386 - canvas_.MeasureText("油盐常备", 1), 108, "油盐常备", 1, 0);

        const int show_ing = std::min<int>(4, static_cast<int>(mv.matched.size()));
        for (int i = 0; i < show_ing; ++i) {
            const auto& m = mv.matched[i];
            const int ix = (i % 2 == 0) ? 16 : 208;
            const int iy = 136 + (i / 2) * 30;
            if (m.state == MatchState::kMissing) {
                canvas_.Circle(ix + 5, iy + 8, 4, 0, false);
                canvas_.DrawText(ix + 16, iy, m.name + m.amount + " (缺)", 1, 0, 162);
            } else {
                canvas_.Circle(ix + 5, iy + 8, 4, 0, true);
                const std::string lbl = m.name + m.amount +
                                        (m.state == MatchState::kHaveUrgent ? " · 临期" : "");
                canvas_.DrawText(ix + 16, iy, lbl, 1, 0, 162);
            }
        }

        const std::string b1 = mv.missing_count > 0
                                   ? ("缺" + std::to_string(mv.missing_count) + "样加采购")
                                   : "缺料已齐";
        const std::string btns[3] = {"开始做菜", b1, "换一道"};
        constexpr int bx[3] = {12, 140, 294};
        constexpr int bw[3] = {120, 146, 94};
        for (int i = 0; i < 3; ++i) {
            DrawButton(bx[i], 214, bw[i], 42, btns[i], UiIcon::kPot, false, i == btn_idx);
        }
        DrawKeyLegend("换按钮", "确认/按住说");
        return Flush1bpp(force_full);
    }

    // Title row: dish name + match chip
    const std::string match_s = "匹配 " + std::to_string(mv.match_pct) + "%";
    const int mw = canvas_.MeasureText(match_s, 1) + 30;
    canvas_.DrawText(12, 36, mv.recipe.title, 2, 0, 388 - mw - 24);
    canvas_.FillRoundRectR(388 - mw, 40, mw, 22, 11, 0);
    canvas_.DrawIcon16(UiIcon::kHeart, 394 - mw, 43, 15);
    canvas_.DrawText(388 - mw + 24, 42, match_s, 1, 15);

    const std::string counter = std::to_string(r_i + 1) + "/" + std::to_string(recipes.size()) + "道";
    const int cw = canvas_.MeasureText(counter, 1) + 12;
    canvas_.DrawClockIcon(13, 74, 0);
    canvas_.DrawText(27, 70, std::to_string(mv.recipe.time_min) + "分钟 · " + mv.recipe.summary,
                     1, 0, 388 - cw - 12 - 27);
    DrawChip(388 - cw, 69, counter, false);

    // Section tag + dotted rule
    canvas_.RoundRectR(10, 96, 98, 22, 11, 0, 1);
    canvas_.DrawIcon16(UiIcon::kLeaf, 16, 99, 0);
    canvas_.DrawText(36, 98, "食材对照", 1, 0);
    canvas_.DottedHLine(114, 107, 194, 3, 0);
    canvas_.DrawText(314, 98, "油盐常备", 1, 0);

    const int show_ing = std::min<int>(4, static_cast<int>(mv.matched.size()));
    for (int i = 0; i < show_ing; ++i) {
        const auto& m = mv.matched[i];
        const int ix = (i % 2 == 0) ? 14 : 206;
        const int iy = 126 + (i / 2) * 30;
        if (m.state == MatchState::kMissing) {
            for (int a = 0; a < 32; ++a) {  // dotted ring = "not in fridge"
                static constexpr int kRing[32][2] = {
                    {8, 0}, {8, 2}, {7, 3}, {7, 5}, {6, 6}, {5, 7}, {3, 7}, {2, 8},
                    {0, 8}, {-2, 8}, {-3, 7}, {-5, 7}, {-6, 6}, {-7, 5}, {-7, 3}, {-8, 2},
                    {-8, 0}, {-8, -2}, {-7, -3}, {-7, -5}, {-6, -6}, {-5, -7}, {-3, -7}, {-2, -8},
                    {0, -8}, {2, -8}, {3, -7}, {5, -7}, {6, -6}, {7, -5}, {7, -3}, {8, -2}};
                if ((a & 1) == 0) canvas_.Pixel(ix + 8 + kRing[a][0], iy + 9 + kRing[a][1], 0);
            }
            canvas_.DrawCross(ix + 5, iy + 6, 0);
            canvas_.DrawText(ix + 22, iy + 1, m.name + m.amount, 1, 0, 118);
            canvas_.DrawText(ix + 22 + std::min(118, canvas_.MeasureText(m.name + m.amount, 1)) + 4,
                             iy + 1, "缺", 1, 0);
        } else {
            canvas_.Circle(ix + 8, iy + 9, 8, 0, true);
            canvas_.DrawCheck(ix + 4, iy + 4, 15);
            std::string lbl = m.name + m.amount;
            canvas_.DrawText(ix + 22, iy + 1, lbl, 1, 0, 120);
            if (m.state == MatchState::kHaveUrgent) {
                DrawChip(ix + 22 + std::min(120, canvas_.MeasureText(lbl, 1)) + 4, iy, "临期", true);
            }
        }
    }

    const std::string b1 = mv.missing_count > 0
                               ? ("缺" + std::to_string(mv.missing_count) + "样加采购")
                               : "缺料已齐";
    const std::string btns[3] = {"开始做菜", b1, "换一道"};
    const UiIcon icons[3] = {UiIcon::kPot, UiIcon::kBasket, UiIcon::kRefresh};
    constexpr int bx[3] = {8, 136, 294};
    constexpr int bw[3] = {122, 152, 98};
    for (int i = 0; i < 3; ++i) {
        DrawButton(bx[i], 212, bw[i], 42, btns[i], icons[i], true, i == btn_idx);
    }

    DrawKeyLegend("换按钮", "确认/按住说");
    return Flush1bpp(force_full);
}

// ============================================================================
// Page 3: 29px Large-Print Cooking Guide (step chip, progress dots, ONE button)
// ============================================================================
esp_err_t FridgeUi::RenderCookPage(const UiTopBar& bar,
                                   const RecipeMatchView* recipe,
                                   int step_idx,
                                   const std::vector<KitchenTimer>& timers,
                                   bool force_full) {
    canvas_.SetMode4bpp(false);
    canvas_.Clear(15);
    DrawTopNav(FridgeTab::kCook, bar);

    const bool has_recipe = (recipe != nullptr && !recipe->recipe.steps.empty());
    int clamped = 0, total_steps = 0, step_timer_min = 0;
    bool is_last_step = false;

    if (has_recipe) {
        total_steps = static_cast<int>(recipe->recipe.steps.size());
        clamped = std::clamp(step_idx, 0, total_steps - 1);
        const auto& st = recipe->recipe.steps[clamped];
        step_timer_min = st.timer_min;
        is_last_step = (clamped == total_steps - 1);

        bool any_ring = false, any_act = false;
        for (const auto& t : timers) {
            if (t.ringing) any_ring = true;
            if (t.active) any_act = true;
        }
        std::string ts;
        if (any_ring) ts = "计时到!";
        else if (any_act) ts = "计时进行中";
        else if (step_timer_min > 0) ts = "建议" + std::to_string(step_timer_min) + "分钟";

        if (is_minimal()) {
            std::string hdr = recipe->recipe.title + " · 第 " + std::to_string(clamped + 1) + "/" +
                              std::to_string(total_steps) + " 步";
            if (!st.heat.empty()) hdr += " · " + st.heat;
            const int tsw = ts.empty() ? 0 : (canvas_.MeasureText(ts, 1) + 12);
            canvas_.DrawText(14, 38, hdr, 1, 0, 372 - tsw);
            if (!ts.empty()) canvas_.DrawText(386 - canvas_.MeasureText(ts, 1), 38, ts, 1, 0);
            canvas_.DottedHLine(14, 61, 372, 4, 0);

            constexpr int kTextW = 356;
            const int est_w = canvas_.MeasureText(st.text, 2);
            const int est_lines = std::clamp((est_w + kTextW - 1) / kTextW, 1, 3);
            const int line_h = 35;
            const int block_h = (est_lines - 1) * line_h + 29;
            const int text_y = 66 + std::max(8, (118 - block_h) / 2);
            canvas_.DrawWrappedText(22, text_y, kTextW, 3, line_h, st.text, 2, 0);

            const int dot_gap = 18;
            const int dots_w = (total_steps - 1) * dot_gap;
            int dx = (PixelCanvas::kWidth - dots_w) / 2;
            for (int i = 0; i < total_steps; ++i, dx += dot_gap) {
                if (i == clamped) canvas_.FillRoundRectR(dx - 6, 194, 14, 5, 2, 0);
                else canvas_.Circle(dx, 196, 3, 0, i < clamped);
            }
        } else {
            canvas_.DrawIcon16(UiIcon::kPot, 8, 35, 0);
            canvas_.DrawText(28, 35, recipe->recipe.title, 1, 0, 100);
            const std::string sp = "第" + std::to_string(clamped + 1) + "/" + std::to_string(total_steps) + "步";
            int cx = 132;
            cx += DrawChip(cx, 34, sp, true) + 4;
            if (!st.heat.empty()) {
                const int hw = canvas_.MeasureText(st.heat, 1) + 30;
                canvas_.RoundRectR(cx, 34, hw, 19, 9, 0, 1);
                canvas_.DrawIcon16(UiIcon::kFlame, cx + 5, 35, 0);
                canvas_.DrawText(cx + 23, 35, st.heat, 1, 0);
            }
            if (!ts.empty()) {
                const int tsw = canvas_.MeasureText(ts, 1);
                canvas_.DrawText(392 - tsw, 35, ts, 1, 0);
            }

            constexpr int kCardX = 12, kCardY = 64, kCardW = 376, kCardH = 118;
            canvas_.FillPatternRoundRectR(kCardX + 3, kCardY + 3, kCardW, kCardH, 10, UiFill::kDots25);
            canvas_.FillRoundRectR(kCardX, kCardY, kCardW, kCardH, 10, 15);
            canvas_.RoundRectR(kCardX, kCardY, kCardW, kCardH, 10, 0, 1);

            constexpr int kTextW = kCardW - 28;
            const int est_w = canvas_.MeasureText(st.text, 2);
            const int est_lines = std::clamp((est_w + kTextW - 1) / kTextW, 1, 3);
            const int line_h = 33;
            const int block_h = (est_lines - 1) * line_h + 29;
            const int text_y = kCardY + std::max(8, (kCardH - block_h) / 2);
            canvas_.DrawWrappedText(kCardX + 14, text_y, kTextW, 3, line_h, st.text, 2, 0);

            const int dot_gap = 16;
            const int dots_w = (total_steps - 1) * dot_gap;
            int dx = (PixelCanvas::kWidth - dots_w) / 2;
            for (int i = 0; i < total_steps; ++i, dx += dot_gap) {
                if (i < clamped) canvas_.Circle(dx, 199, 4, 0, true);
                else if (i == clamped) { canvas_.Circle(dx, 199, 6, 0, false); canvas_.Circle(dx, 199, 3, 0, true); }
                else canvas_.Circle(dx, 199, 4, 0, false);
            }
        }
    } else {
        if (is_minimal()) {
            canvas_.DrawTextCentered(116, "先去「食谱」选一道菜", 1, 0);
        } else {
            canvas_.DrawMascotPixel(170, 52, 3, false);
            canvas_.DrawTextCentered(126, "先回菜单去[食谱]选一道菜吧~", 1, 0);
        }
    }

    // A step timer started from this page is labelled with the recipe title
    bool recipe_timer_running = false;
    if (has_recipe) {
        for (const auto& t : timers) {
            if (t.active && t.label == recipe->recipe.title) recipe_timer_running = true;
        }
    }

    std::string btn_text;
    UiIcon btn_icon = UiIcon::kClock;
    if (recipe_timer_running && !is_last_step) {
        // No number here on purpose: the live countdown is the top-right pill (partial refresh),
        // a number on this button would freeze between full refreshes.
        btn_text = "计时中，看右上角 · 按OK停止";
        btn_icon = UiIcon::kBell;
    } else if (is_last_step && has_recipe) {
        btn_text = "做好啦! 自动扣减冰箱库存";
        btn_icon = UiIcon::kCheck;
    } else if (step_timer_min > 0) {
        btn_text = "启动本步计时 (" + std::to_string(step_timer_min) + " 分钟)";
    } else {
        btn_text = "启动快捷计时 (5 分钟)";
    }
    if (recipe_timer_running && !is_last_step) {
        if (is_minimal()) {
            canvas_.FillRoundRectR(14, 214, 372, 42, 8, 15);
            canvas_.RoundRectR(14, 214, 372, 42, 8, 0, bar.nav_focus ? 1 : 2);
            const int tw = std::min(350, canvas_.MeasureText(btn_text, 1));
            canvas_.DrawText(14 + (372 - tw) / 2, 226, btn_text, 1, 0, 350);
        } else {
            canvas_.FillPatternRoundRectR(14, 212, 372, 44, 10, UiFill::kDots25);
            canvas_.RoundRectR(14, 212, 372, 44, 10, 0, bar.nav_focus ? 1 : 3);
            const int tw = canvas_.MeasureText(btn_text, 1) + 22;
            const int tx = 14 + (372 - tw) / 2;
            canvas_.FillRoundRectR(tx - 8, 223, tw + 16, 22, 11, 15);
            canvas_.DrawIcon16(btn_icon, tx, 226, 0);
            canvas_.DrawText(tx + 22, 225, btn_text, 1, 0);
        }
    } else {
        DrawButton(14, is_minimal() ? 214 : 212, 372, is_minimal() ? 42 : 44, btn_text, btn_icon, true, !bar.nav_focus);
    }

    DrawKeyLegend("翻步骤", is_last_step ? "完工/按住说"
                                     : (recipe_timer_running ? "停止计时/按住说" : "计时/按住说"));
    return Flush1bpp(force_full);
}

esp_err_t FridgeUi::RenderCookDonePopup(const RecipeMatchView& recipe, int choice_idx) {
    const int px = 40, py = 44, pw = 320, ph = 210;
    DrawPopupFrame(px, py, pw, ph, UiIcon::kPot, recipe.recipe.title + " 做好啦!", Mood::kHappy);

    std::string used;
    for (const auto& m : recipe.matched) {
        if (m.state == MatchState::kHaveUrgent || m.state == MatchState::kHaveFresh) {
            if (!used.empty()) used += "、";
            used += m.name;
        }
    }
    canvas_.DrawWrappedText(px + 16, py + 46, pw - 32, 2, 21,
                            used.empty() ? "本菜未关联到冰箱库存项。"
                                         : ("顺手从冰箱扣减: " + used + "?"),
                            1, 0);

    const char* opts[2] = {"是的，帮我扣减冰箱库存", "不用扣，先保留"};
    const UiIcon icons[2] = {UiIcon::kCheck, UiIcon::kHeart};
    for (int i = 0; i < 2; ++i) {
        DrawButton(px + 16, py + 94 + i * 38, pw - 32, 31, opts[i], icons[i], true, i == choice_idx);
    }

    canvas_.DottedHLine(px + 12, py + ph - 33, pw - 24, 3, 0);
    DrawKeyHints(px + 16, py + ph - 26, {{"上", "退出"}, {"OK", "确认"}, {"下", "换选项"}});
    return FlushRect1bpp({px - 6, py - 6, pw + 16, ph + 16});
}

// ============================================================================
// Page 4: Shopping List (notepad rows + commit button)
// ============================================================================
esp_err_t FridgeUi::RenderShoppingPage(const UiTopBar& bar,
                                       const std::vector<ShoppingItem>& list,
                                       int selected_idx,
                                       bool force_full,
                                       bool has_nfc) {
    canvas_.SetMode4bpp(false);
    canvas_.Clear(15);
    DrawTopNav(FridgeTab::kShopping, bar);

    int pending = 0, checked = 0;
    for (const auto& s : list) { if (s.checked) checked++; else pending++; }

    if (is_minimal()) {
        std::string shdr = "采购清单";
        if (pending > 0) shdr += has_nfc ? " · NFC 一碰带走" : " · 可扫码带走";
        canvas_.DrawText(14, 36, shdr, 1, 0, 268);
        const std::string cnt = "待买 " + std::to_string(pending) + " · 勾 " + std::to_string(checked);
        canvas_.DrawText(386 - canvas_.MeasureText(cnt, 1), 36, cnt, 1, 0);
        if (list.empty()) {
            canvas_.DrawTextCentered(116, "采购清单为空", 2, 0);
            canvas_.HLine(184, 154, 32, 0);
            canvas_.DrawTextCentered(172, "按住 OK 说“买牛奶”就能添加", 1, 0);
            DrawKeyLegend("刷新", "按住说");
            return Flush1bpp(force_full);
        }
        canvas_.HLine(14, 58, 372, 0);
    } else {
        canvas_.DrawIcon16(UiIcon::kBasket, 8, 34, 0);
        canvas_.DrawText(28, 34, "逛超市清单", 1, 0);
        if (pending > 0) {
            DrawChip(122, 33, has_nfc ? "NFC 一碰带走" : "可扫码带走", false);
        }
        const std::string cnt = "待买" + std::to_string(pending) + " · 勾" + std::to_string(checked);
        canvas_.DrawText(392 - canvas_.MeasureText(cnt, 1), 34, cnt, 1, 0);

        if (list.empty()) {
            canvas_.DrawMascotPixel(170, 64, 3, false);
            canvas_.DrawTextCentered(140, "采购清单空空的~", 1, 0);
            canvas_.RoundRectR(46, 172, 308, 34, 17, 0, 1);
            canvas_.DrawIcon16(UiIcon::kMic, 60, 181, 0);
            canvas_.DrawText(82, 180, "按住 OK 说“买牛奶”就能添加", 1, 0, 262);
            DrawKeyLegend("刷新", "按住说");
            return Flush1bpp(force_full);
        }
    }

    const int n = static_cast<int>(list.size());
    const int total_sel = n + 2;
    const bool has_sel = selected_idx >= 0;
    const int clamped = std::clamp(selected_idx, 0, total_sel - 1);

    constexpr int kMaxVis = 4;
    constexpr int kRowH = 36;
    const int vis_start = (clamped < n && clamped >= kMaxVis) ? (clamped - kMaxVis + 1) : 0;
    const int vis_end = std::min(n, vis_start + kMaxVis);

    // Notepad / minimalist rows
    if (!is_minimal()) {
        for (int r = 0; r < kMaxVis; ++r) {
            canvas_.DottedHLine(10, 58 + (r + 1) * kRowH - 3, 380, 2, 0);
        }
    }

    for (int i = vis_start; i < vis_end; ++i) {
        const auto& s = list[i];
        const int ry = (is_minimal() ? 66 : 58) + (i - vis_start) * kRowH;
        const bool sel = has_sel && (i == clamped);
        const bool next_sel = has_sel && (i + 1 == clamped);
        const uint8_t fg = sel ? 15 : 0;

        if (sel) {
            canvas_.FillRoundRectR(is_minimal() ? 12 : 8, ry, is_minimal() ? 376 : 384,
                                   is_minimal() ? 32 : 30, 8, 0);
            if (!is_minimal()) canvas_.DrawPointer(14, ry + 11, 15);
        } else if (is_minimal() && !next_sel && i + 1 < vis_end) {
            canvas_.DottedHLine(16, ry + kRowH - 2, 368, 4, 0);
        }
        const int bx = is_minimal() ? 24 : (sel ? 28 : 16);
        const int cb = is_minimal() ? 14 : 16;
        const int cy_cb = is_minimal() ? (ry + 9) : (ry + 7);
        if (s.checked) {
            canvas_.FillRoundRectR(bx, cy_cb, cb, cb, 3, fg);
            canvas_.DrawCheck(bx + 3, cy_cb + 2, sel ? 0 : 15);
        } else {
            canvas_.RoundRectR(bx, cy_cb, cb, cb, 3, fg, is_minimal() ? 1 : 2);
        }

        const int tx = bx + cb + 10;
        canvas_.DrawText(tx, ry + 7, s.name + " " + s.qty, 1, fg, 196);
        if (s.checked) {
            const int nw = std::min(196, canvas_.MeasureText(s.name + " " + s.qty, 1));
            canvas_.HLine(tx - 2, ry + 16, nw + 4, fg);  // crossed off
        }
        const std::string right_s = s.checked ? "已买" : s.source;
        if (!right_s.empty()) {
            const int rw = std::min(130, canvas_.MeasureText(right_s, 1));
            canvas_.DrawText((is_minimal() ? 376 : 382) - rw, ry + 7, right_s, 1, fg, 130);
        }
    }
    if (vis_end < n) {
        for (int i = 0; i < 5; ++i) canvas_.HLine(380 + i, 198 + i, 9 - 2 * i, 0);  // more below
    }

    const bool sel_commit = has_sel && (clamped == n);
    const bool sel_takeaway = has_sel && (clamped == n + 1);
    DrawButton(8, 212, 230, 44, "已勾(" + std::to_string(checked) + ") 放进冰箱",
               UiIcon::kFridge, true, sel_commit);
    DrawButton(246, 212, 146, 44, "带走清单",
               UiIcon::kBasket, true, sel_takeaway);

    DrawKeyLegend("选择", sel_commit ? "入库/按住说" : (sel_takeaway ? "带走/按住说" : "勾选/按住说"));
    return Flush1bpp(force_full);
}

esp_err_t FridgeUi::RenderShoppingQrPopup(const std::string& qr_payload,
                                          bool online_mode,
                                          bool has_nfc,
                                          const std::string& ip,
                                          int pending_count) {
    const int px = 18, py = 22, pw = 364, ph = 248;
    DrawPopupFrame(px, py, pw, ph, UiIcon::kBasket, "手机带走采购清单", Mood::kNone);

    // Left: scannable QR code in a clean frame
    canvas_.RoundRectR(px + 12, py + 48, 144, 144, 8, 0, 1);
    canvas_.DrawQrCode(px + 18, py + 54, 132, qr_payload);

    // Right: step-by-step hints
    const int rx = px + 166;
    const int rw = pw - 178;
    if (online_mode) {
        DrawChip(rx, py + 50, has_nfc ? "碰NFC 或 扫左码" : "手机扫码直达", true);
        canvas_.DrawText(rx, py + 78, "待买 " + std::to_string(pending_count) + " 样食材", 1, 0, rw);
        canvas_.DrawText(rx, py + 102, "1. 打开采购清单页", 1, 0, rw);
        canvas_.DrawText(rx, py + 124, "2. 点“生成购物卡”", 1, 0, rw);
        canvas_.DrawText(rx, py + 146, "3. 长按图片存相册", 1, 0, rw);
        canvas_.DottedHLine(rx, py + 170, rw, 3, 0);
        canvas_.DrawText(rx, py + 176, "IP:" + ip, 1, 0, rw);
    } else {
        DrawChip(rx, py + 50, "离线 · 微信扫左码", true);
        canvas_.DrawText(rx, py + 78, "待买 " + std::to_string(pending_count) + " 样食材", 1, 0, rw);
        canvas_.DrawWrappedText(rx, py + 104, rw, 4, 22,
                                "未连Wi-Fi，用微信扫左码可直接复制或转发给家人",
                                1, 0);
    }

    canvas_.DottedHLine(px + 12, py + ph - 34, pw - 24, 3, 0);
    DrawKeyHints(px + 16, py + ph - 27, {{"任意键", "关闭返回"}});
    return FlushRect1bpp({px - 6, py - 6, pw + 16, ph + 16});
}

// ============================================================================
// Page 5: Settings & QR
// ============================================================================
esp_err_t FridgeUi::RenderSettingsPage(const UiTopBar& bar,
                                       const AppConfig& cfg,
                                       const std::string& ip,
                                       const std::string& qr_payload,
                                       const std::string& qr_caption,
                                       const std::string& ap_password,
                                       const ZectrixPowerSnapshot& pwr,
                                       int selected_idx,
                                       bool force_full) {
    canvas_.SetMode4bpp(false);
    canvas_.Clear(15);
    DrawTopNav(FridgeTab::kSettings, bar);

    // Left: QR in a rounded frame (read-only)
    if (is_minimal()) {
        canvas_.RoundRectR(12, 40, 142, 142, 8, 0, 1);
        canvas_.DrawQrCode(21, 49, 124, qr_payload);
        canvas_.DrawText(12, 190, qr_caption, 1, 0, 146);
        canvas_.DrawText(12, 212, bar.ap_mode ? "ZecTrix-Fridge" : ("IP " + ip), 1, 0, 146);
        canvas_.DrawText(12, 234, bar.ap_mode ? ("密码 " + ap_password) : std::string("网页可切换外观主题"), 1, 0, 146);
    } else {
        canvas_.RoundRectR(8, 36, 148, 154, 10, 0, 1);
        const int cap_w = std::min(132, canvas_.MeasureText(qr_caption, 1));
        canvas_.FillRect(82 - cap_w / 2 - 6, 30, cap_w + 12, 14, 15);
        canvas_.DrawText(82 - cap_w / 2, 30, qr_caption, 1, 0, 132);
        canvas_.DrawQrCode(20, 52, 124, qr_payload);
        canvas_.DrawText(12, 196, bar.ap_mode ? "ZecTrix-Fridge" : ("IP:" + ip), 1, 0, 146);
        canvas_.DrawText(12, 217, bar.ap_mode ? ("密码:" + ap_password) : std::string("手机扫码管理冰箱"), 1, 0, 146);
        canvas_.DrawText(12, 238, bar.ap_mode ? "连后开192.168.4.1" : "可配Wi-Fi与云端AI", 1, 0, 146);
    }

    // Right: read-only status lines
    const std::string net_s =
        bar.ap_mode ? "AP配网热点中"
                    : ((cfg.wifi_ssid.empty() ? std::string("未配置") : cfg.wifi_ssid) +
                       (bar.wifi_ok ? " · 已连" : " · 断开"));
    const bool is_pub_xz = cfg.ota_url.empty() ||
                           cfg.ota_url.find("tenclass.net") != std::string::npos;
    const bool is_lan_xz = cfg.ota_url.find("192.168.") != std::string::npos ||
                           cfg.ota_url.find("10.") != std::string::npos ||
                           cfg.ota_url.find("172.") != std::string::npos;
    const std::string ai_s = is_pub_xz ? "小智官方 (免Key)"
                                       : (is_lan_xz ? "局域网小智 (MCP)" : "自定义小智服务");
    char bbuf[64];
    std::snprintf(bbuf, sizeof(bbuf), "%u%% · %s", pwr.battery_percent,
                  pwr.charge.power_present ? "USB供电" : "电池省电");

    if (is_minimal()) {
        canvas_.DrawText(170, 38, "网络  " + net_s, 1, 0, 218);
        canvas_.DrawText(170, 60, "语音  " + ai_s, 1, 0, 218);
        canvas_.DrawText(170, 82, "电源  " + std::string(bbuf), 1, 0, 218);
        canvas_.HLine(170, 104, 218, 0);
    } else {
        canvas_.DrawIcon16(UiIcon::kWifi, 168, 36, 0);
        canvas_.DrawText(188, 36, net_s, 1, 0, 204);
        canvas_.DrawIcon16(UiIcon::kMic, 168, 57, 0);
        canvas_.DrawText(188, 57, ai_s, 1, 0, 204);
        canvas_.DrawBatteryIcon(167, 81, pwr.battery_percent, pwr.charge.charging, 0, 15);
        canvas_.DrawText(188, 78, bbuf, 1, 0, 204);
        canvas_.DottedHLine(168, 102, 222, 3, 0);
    }

    const std::string ctrls[4] = {
        "语音说明 / 绑定码",
        bar.ap_mode ? "重连家庭Wi-Fi" : "开启AP配网热点",
        "立即小憩 (灰阶封面)",
        "重启 / 清屏关机",
    };
    const UiIcon icons[4] = {UiIcon::kMic, UiIcon::kWifi, UiIcon::kMoon, UiIcon::kPower};
    const int clamped = std::clamp(selected_idx, 0, 3);
    for (int i = 0; i < 4; ++i) {
        DrawButton(166, 110 + i * 38, 226, 32, ctrls[i], icons[i], true,
                   selected_idx >= 0 && i == clamped);
    }

    DrawKeyLegend("选择", "执行/按住说");
    return Flush1bpp(force_full);
}

// ============================================================================
// Voice feedback popups (never a mystery beep!)
// ============================================================================
esp_err_t FridgeUi::RenderVoiceResultPopup(bool success,
                                           const std::string& heard,
                                           const std::string& action_detail,
                                           bool can_undo) {
    const int px = 24, py = 32, pw = 352, ph = 230;
    if (success) {
        DrawPopupFrame(px, py, pw, ph, UiIcon::kCheck, "听懂啦! 已为你执行", Mood::kHappy);
        canvas_.FillRoundRectR(px + 14, py + 48, pw - 28, 26, 13, 15);
        canvas_.RoundRectR(px + 14, py + 48, pw - 28, 26, 13, 0, 1);
        canvas_.DrawIcon16(UiIcon::kMic, px + 22, py + 53, 0);
        canvas_.DrawText(px + 44, py + 52, "“" + heard + "”", 1, 0, pw - 72);
        canvas_.DrawText(px + 18, py + 84, "已执行:", 1, 0);
        canvas_.DrawWrappedText(px + 18, py + 106, pw - 36, 3, 22, action_detail, 1, 0);
        canvas_.DottedHLine(px + 14, py + ph - 34, pw - 28, 3, 0);
        if (can_undo) {
            DrawKeyHints(px + 16, py + ph - 27, {{"上", "撤销刚才操作"}, {"OK", "知道了"}});
        } else {
            DrawKeyHints(px + 16, py + ph - 27, {{"OK", "知道了 (5秒自动关)"}});
        }
    } else {
        DrawPopupFrame(px, py, pw, ph, UiIcon::kCross, "语音未执行", Mood::kWorried);
        canvas_.DrawWrappedText(px + 16, py + 48, pw - 32, 3, 21, action_detail, 1, 0);
        canvas_.DottedHLine(px + 14, py + 114, pw - 28, 3, 0);
        canvas_.DrawText(px + 16, py + 122, "按住 OK 听“叮”后自然说中文:", 1, 0);
        canvas_.DrawIcon16(UiIcon::kSpark, px + 16, py + 145, 0);
        canvas_.DrawText(px + 36, py + 145, "放两盒牛奶5天过期，番茄吃完了", 1, 0, pw - 52);
        canvas_.DrawIcon16(UiIcon::kSpark, px + 16, py + 167, 0);
        canvas_.DrawText(px + 36, py + 167, "定时15分钟 / 推荐一道菜", 1, 0, pw - 52);
        canvas_.DottedHLine(px + 14, py + ph - 34, pw - 28, 3, 0);
        DrawKeyHints(px + 16, py + ph - 27, {{"任意键", "关闭"}});
    }
    return FlushRect1bpp({px - 6, py - 6, pw + 16, ph + 16});
}

esp_err_t FridgeUi::RenderVoiceHelpPopup() {
    const int px = 20, py = 18, pw = 360, ph = 244;
    DrawPopupFrame(px, py, pw, ph, UiIcon::kMic, "小智语音 + 冰箱 MCP", Mood::kHappy);

    canvas_.DrawText(px + 16, py + 46, "免充值免 Key：默认连 xiaozhi.me 免费云", 1, 0, pw - 32);
    canvas_.DrawText(px + 16, py + 67, "也可扫码改连自家局域网 NAS 小智", 1, 0, pw - 32);
    canvas_.DottedHLine(px + 14, py + 90, pw - 28, 3, 0);

    canvas_.DrawText(px + 16, py + 97, "按住 OK 听“叮”直接说:", 1, 0, pw - 32);
    const char* lines[4] = {
        "放两盒牛奶5天过期，排骨放冷冻",
        "番茄吃光了，顺手把小葱加采购单",
        "定时15分钟炖排骨 / 下一步",
        "用冰箱里的菜推荐一道快手菜",
    };
    for (int i = 0; i < 4; ++i) {
        canvas_.DrawIcon16(UiIcon::kSpark, px + 16, py + 120 + i * 22, 0);
        canvas_.DrawText(px + 36, py + 120 + i * 22, lines[i], 1, 0, pw - 52);
    }

    canvas_.DottedHLine(px + 14, py + ph - 33, pw - 28, 3, 0);
    DrawKeyHints(px + 16, py + ph - 26, {{"上", "退出返回"}, {"OK", "检查连接/绑定码"}});
    return FlushRect1bpp({px - 6, py - 6, pw + 16, ph + 16});
}

esp_err_t FridgeUi::RenderActivationPopup(const std::string& code, const std::string& status_line) {
    const int px = 28, py = 30, pw = 344, ph = 232;
    DrawPopupFrame(px, py, pw, ph, UiIcon::kHeart, "首次使用 · 小智设备绑定", Mood::kHappy);

    canvas_.DrawTextCentered(py + 48, "用手机或电脑打开 xiaozhi.me", 1, 0);
    canvas_.DrawTextCentered(py + 69, "登录后输入下方 6 位绑定码:", 1, 0);

    // Big 29px code box
    if (!is_minimal()) {
        canvas_.FillPatternRoundRectR(px + 66, py + 98, pw - 128, 52, 10, UiFill::kDots25);
    }
    canvas_.FillRoundRectR(px + 62, py + 94, pw - 128, 52, 10, 15);
    canvas_.RoundRectR(px + 62, py + 94, pw - 128, 52, 10, 0, is_minimal() ? 1 : 2);
    canvas_.DrawTextCentered(py + 105, code.empty() ? "------" : code, 2, 0);

    canvas_.DrawTextCentered(py + 160, status_line.empty() ? "输入后按 OK 即可完成绑定" : status_line, 1, 0);
    canvas_.DottedHLine(px + 14, py + ph - 34, pw - 28, 3, 0);
    DrawKeyHints(px + 18, py + ph - 27, {{"上", "稍后再说"}, {"OK", "我已输入，验证"}});
    return FlushRect1bpp({px - 6, py - 6, pw + 16, ph + 16});
}

esp_err_t FridgeUi::ShowQaPopup(const std::string& title, const std::string& answer) {
    const int px = 28, py = 42, pw = 344, ph = 210;
    DrawPopupFrame(px, py, pw, ph, UiIcon::kSpark, title, Mood::kHappy);
    canvas_.DrawWrappedText(px + 16, py + 48, pw - 32, 5, 22, answer, 1, 0);
    canvas_.DottedHLine(px + 14, py + ph - 33, pw - 28, 3, 0);
    DrawKeyHints(px + 16, py + ph - 26, {{"任意键", "关闭"}});
    return FlushRect1bpp({px - 6, py - 6, pw + 16, ph + 16});
}

esp_err_t FridgeUi::RenderLoadingPopup(const std::string& title, const std::string& detail) {
    const int px = 36, py = 70, pw = 328, ph = 156;
    DrawPopupFrame(px, py, pw, ph, UiIcon::kPot, title, Mood::kHappy);
    canvas_.DrawWrappedText(px + 18, py + 50, pw - 36, 3, 23, detail, 1, 0);
    // "simmering" dots
    for (int i = 0; i < 3; ++i) canvas_.Circle(px + pw / 2 - 16 + i * 16, py + ph - 20, 3, 0, true);
    return FlushRect1bpp({px - 6, py - 6, pw + 16, ph + 16});
}

esp_err_t FridgeUi::RenderSleepTimerConfirmPopup(const std::string& timer_desc, int choice_idx,
                                                 const char* action_label) {
    const int px = 34, py = 42, pw = 332, ph = 212;
    DrawPopupFrame(px, py, pw, ph, UiIcon::kClock, "厨房倒计时运行中!", Mood::kWorried);

    const std::string act = (action_label && action_label[0] != '\0') ? action_label : "休眠";
    canvas_.DrawWrappedText(
        px + 16, py + 46, pw - 32, 2, 21,
        "正在计时（" + timer_desc + "），" + act + "后将无法到点响铃。",
        1, 0);

    const std::string opts[2] = {"继续做菜，暂不" + act, "取消全部计时并" + act};
    const UiIcon act_icon =
        (act == "重启") ? UiIcon::kRefresh : ((act == "关机") ? UiIcon::kPower : UiIcon::kMoon);
    const UiIcon icons[2] = {UiIcon::kPot, act_icon};
    for (int i = 0; i < 2; ++i) {
        DrawButton(px + 16, py + 96 + i * 38, pw - 32, 31, opts[i], icons[i], true, i == choice_idx);
    }

    canvas_.DottedHLine(px + 12, py + ph - 33, pw - 24, 3, 0);
    DrawKeyHints(px + 16, py + ph - 26, {{"上", "返回"}, {"OK", "确认"}, {"下", "换选项"}});
    return FlushRect1bpp({px - 6, py - 6, pw + 16, ph + 16});
}

esp_err_t FridgeUi::RenderPowerMenuPopup(int choice_idx) {
    const int px = 44, py = 46, pw = 312, ph = 206;
    DrawPopupFrame(px, py, pw, ph, UiIcon::kPower, "重启或关机", Mood::kNone);

    const char* opts[3] = {"重启设备", "清屏并关机", "取消返回"};
    const UiIcon icons[3] = {UiIcon::kRefresh, UiIcon::kPower, UiIcon::kCross};
    const int clamped = std::clamp(choice_idx, 0, 2);
    for (int i = 0; i < 3; ++i) {
        DrawButton(px + 20, py + 48 + i * 40, pw - 40, 32, opts[i], icons[i], true, i == clamped);
    }

    canvas_.DottedHLine(px + 14, py + ph - 33, pw - 28, 3, 0);
    DrawKeyHints(px + 16, py + ph - 26, {{"上", "取消"}, {"下", "切换"}, {"OK", "确认"}});
    return FlushRect1bpp({px - 6, py - 6, pw + 16, ph + 16});
}

esp_err_t FridgeUi::ShowAlarmScreen(const std::vector<KitchenTimer>& timers) {
    canvas_.SetMode4bpp(false);
    canvas_.Clear(15);

    std::string label = "厨房倒计时已结束";
    for (const auto& t : timers) {
        if (t.ringing) { label = t.label + " 好啦!"; break; }
    }

    if (is_minimal()) {
        canvas_.FillRect(0, 0, PixelCanvas::kWidth, 96, 0);
        canvas_.DrawTextCentered(33, "计时结束", 2, 15);
        canvas_.DrawTextCentered(138, label, 2, 0);
        canvas_.HLine(160, 186, 80, 0);
        const std::string hint = "按任意键关闭闹铃";
        const int hw = canvas_.MeasureText(hint, 1) + 32;
        canvas_.RoundRectR((PixelCanvas::kWidth - hw) / 2, 214, hw, 32, 16, 0, 1);
        canvas_.DrawTextCentered(221, hint, 1, 0);
        return Flush1bpp(true);
    }

    // Tablecloth frame
    canvas_.FillPattern(0, 0, PixelCanvas::kWidth, PixelCanvas::kHeight, UiFill::kGingham);
    canvas_.FillRoundRectR(18, 16, 364, 268, 14, 15);
    canvas_.RoundRectR(18, 16, 364, 268, 14, 0, 3);

    canvas_.DrawMascotPixel(170, 30, 3, true);
    canvas_.DrawIcon16(UiIcon::kBell, 128, 52, 0);
    canvas_.DrawIcon16(UiIcon::kBell, 256, 52, 0);
    // Ringing sound-wave arcs outside both bells
    for (int i = -4; i <= 4; ++i) {
        const int dx = (std::abs(i) >= 3) ? 1 : 0;
        canvas_.Pixel(122 + dx, 59 + i, 0);
        canvas_.Pixel(278 - dx, 59 + i, 0);
        if (std::abs(i) <= 2) {
            canvas_.Pixel(118 + (std::abs(i) == 2 ? 1 : 0), 59 + i, 0);
            canvas_.Pixel(282 - (std::abs(i) == 2 ? 1 : 0), 59 + i, 0);
        }
    }
    canvas_.DrawTextCentered(98, "时间到啦!", 2, 0);

    const int lw = std::min(320, canvas_.MeasureText(label, 2) + 40);
    const int lx = (PixelCanvas::kWidth - lw) / 2;
    canvas_.FillRoundRectR(lx, 146, lw, 46, 23, 0);
    canvas_.DrawTextCentered(154, label, 2, 15);

    canvas_.DottedHLine(60, 214, 280, 3, 0);
    const std::string hint = "按任意键关闭闹铃";
    const int hw = canvas_.MeasureText(hint, 1) + 30;
    canvas_.RoundRectR((PixelCanvas::kWidth - hw) / 2, 230, hw, 28, 14, 0, 1);
    canvas_.DrawTextCentered(235, hint, 1, 0);
    return Flush1bpp(true);
}
