#include "fridge_ui.h"

#include <algorithm>
#include <cstdio>

// ============================================================================
// "Warm kitchen" visual language for the 400x300 EPD
//  - Tablecloth gingham trims (dithered in 1bpp, real grays in 4bpp)
//  - Soft rounded cards (r=6..10) with dotted drop shadows
//  - 16px pixel-art icons + chunky fridge mascot for empty/feedback states
//  - Selection stays unmistakable: solid black fill + white pointer
// ============================================================================

namespace {

constexpr int kMaxPartialBeforeFull = 15;
constexpr int kMaxPillPartialBeforeFull = 30;

// Region owned by the timer-pill partial refresh (must match DrawStatusArea's footprint)
constexpr int kStatusX = 288;
constexpr int kStatusW = 112;
constexpr int kStatusH = 28;

constexpr int kLegendY = 266;

// Truthful date tag: NEVER fabricate fake expiry dates!
// Uses FridgeStore helpers so unsynced-clock relative ages and synced dates match 100%.
std::string FormatItemDateTag(const FridgeItem& it, int32_t today, bool* is_urgent_out = nullptr) {
    bool has_exp = false;
    const int left = FridgeStore::ComputeItemDaysLeft(it, today, &has_exp);
    if (has_exp) {
        if (is_urgent_out) *is_urgent_out = (left <= 2);
        if (left < 0) return "过期" + std::to_string(-left) + "天";
        if (left == 0) return "今天到期";
        return "剩" + std::to_string(left) + "天";
    }
    if (is_urgent_out) *is_urgent_out = false;
    const int age = FridgeStore::ComputeItemAgeDays(it, today);
    if (age == 0) return "今天放入";
    return "已放" + std::to_string(age) + "天";
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
        const int kw = canvas_.MeasureText(key, 1) + 10;
        canvas_.FillRoundRectR(x, y, kw, 20, 5, 0);
        canvas_.DrawText(x + 5, y + 2, key, 1, 15);
        x += kw + 4;
        canvas_.DrawText(x, y + 2, h.desc, 1, 0);
        x += canvas_.MeasureText(h.desc, 1) + 12;
    }
    return x;
}

void FridgeUi::DrawButton(int x, int y, int w, int h, const std::string& label,
                          UiIcon icon, bool has_icon, bool selected) {
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
    canvas_.FillRect(px - 5, py - 5, pw + 12, ph + 12, 15);
    canvas_.FillPatternRoundRectR(px + 4, py + 4, pw, ph, 10, UiFill::kChecker50);
    canvas_.FillRoundRectR(px, py, pw, ph, 10, 15);
    // Tablecloth header band
    canvas_.FillPatternRoundRectR(px + 2, py + 2, pw - 4, 44, 8, UiFill::kGingham);
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

// Recipe-box style tabs: the active tab is a black folder tab joined to the rule below
void FridgeUi::DrawTopNav(FridgeTab active, const UiTopBar& bar) {
    static constexpr const char* kNames[] = {"库存", "食谱", "烹饪", "采购", "设置"};
    canvas_.FillRect(0, 0, PixelCanvas::kWidth, 31, 15);
    canvas_.FillRect(0, 28, PixelCanvas::kWidth, 2, 0);

    int x = 4;
    for (int i = 0; i < static_cast<int>(FridgeTab::kCount); ++i) {
        const std::string t = kNames[i];
        const int tw = canvas_.MeasureText(t, 1);
        const int tab_w = tw + 16;
        if (i == static_cast<int>(active)) {
            canvas_.FillRoundRectR(x, 2, tab_w, 24, 7, 0);
            canvas_.FillRect(x, 16, tab_w, 12, 0);  // folder tab merges into the rule
            canvas_.DrawText(x + 8, 6, t, 1, 15);
        } else {
            canvas_.DrawText(x + 8, 6, t, 1, 0);
        }
        x += tab_w + 2;
    }
    DrawStatusArea(bar);
}

// Bottom legend with a gingham "tablecloth hem"
void FridgeUi::DrawKeyLegend(const std::string& up_label,
                             const std::string& ok_label,
                             const std::string& down_label,
                             const std::string& right_pill) {
    canvas_.FillRect(0, kLegendY, PixelCanvas::kWidth, PixelCanvas::kHeight - kLegendY, 15);
    canvas_.FillPattern(0, kLegendY, PixelCanvas::kWidth, 6, UiFill::kGingham);
    canvas_.HLine(0, kLegendY + 6, PixelCanvas::kWidth, 0);

    DrawKeyHints(6, 276, {{"上", up_label}, {"OK", ok_label}, {"下", down_label}});

    if (!right_pill.empty()) {
        const int pw = canvas_.MeasureText(right_pill, 1) + 12;
        DrawChip(394 - pw, 276, right_pill, false);
    }
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
// Boot splash
// ============================================================================
esp_err_t FridgeUi::ShowBootSplash(const std::string& subtitle) {
    canvas_.SetMode4bpp(false);
    canvas_.Clear(15);
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

    canvas_.DrawTextCentered(186, "上键切页 · 下键选择 · 按住OK说话", 1, 0);
    canvas_.DottedHLine(120, 216, 160, 3, 0);
    canvas_.DrawTextCentered(228, "把每一样食材都好好吃掉~", 1, 0);
    canvas_.DrawTextCentered(256, "ZECTRIX NOTE4", 1, 0);
    return Flush1bpp(true);
}

// ============================================================================
// Page 0: True 4bpp (16-gray) Standby Poster — tablecloth backdrop, paper cards
// ============================================================================
esp_err_t FridgeUi::RenderStandby4bpp(const std::string& date_str,
                                      const std::string& hm_str,
                                      int battery_pct, bool charging,
                                      const std::vector<FridgeItem>& all_items,
                                      const RecipeMatchView* top_recipe,
                                      const std::vector<ShoppingItem>& shopping,
                                      int wake_hour) {
    if (!epd_) return ESP_ERR_INVALID_STATE;
    canvas_.SetMode4bpp(false);
    canvas_.Clear(15);
    Flush1bpp(true);

    canvas_.SetMode4bpp(true);
    canvas_.Clear(15);
    canvas_.FillPattern(0, 0, PixelCanvas::kWidth, PixelCanvas::kHeight, UiFill::kGingham);

    const int32_t today = FridgeStore::TodayDayNumber();
    int exp_count = 0;
    for (const auto& it : all_items) {
        bool has_exp = false;
        if (FridgeStore::ComputeItemDaysLeft(it, today, &has_exp) <= 2 && has_exp) exp_count++;
    }

    auto paper_card = [&](int x, int y, int w, int h) {
        canvas_.FillRoundRectR(x + 3, y + 3, w, h, 8, 7);
        canvas_.FillRoundRectR(x, y, w, h, 8, 15);
        canvas_.RoundRectR(x, y, w, h, 8, 3, 1);
    };

    // Mascot + speech bubble
    canvas_.FillRoundRectR(6, 4, 48, 48, 12, 15);
    canvas_.DrawMascotPixel(10, 8, 2, exp_count > 0);
    paper_card(60, 6, 222, 42);
    // bubble tail pointing at the mascot
    for (int i = 0; i < 6; ++i) {
        canvas_.HLine(54 + i, 22 + i, 7 - i, 15);
        canvas_.Pixel(54 + i, 22 + i, 3);
    }
    std::string line1 = "冰箱共 " + std::to_string(all_items.size()) + " 样食材~";
    if (!all_items.empty()) {
        bool first_exp = false;
        if (FridgeStore::ComputeItemDaysLeft(all_items[0], today, &first_exp) <= 2 && first_exp) {
            line1 = "提醒: " + all_items[0].name + FormatItemDateTag(all_items[0], today) + "!";
        }
    }
    const std::string line2 = (top_recipe != nullptr)
                                  ? ("今晚吃 " + top_recipe->recipe.title + " 吧~")
                                  : "按 OK 唤醒查看冰箱";
    canvas_.DrawText(70, 9, line1, 1, 0, 204);
    canvas_.DrawText(70, 28, line2, 1, 5, 204);

    // Date / time card
    paper_card(290, 6, 102, 42);
    canvas_.DrawText(298, 9, date_str, 1, 1, 88);
    canvas_.DrawText(298, 28, hm_str, 1, 5, 56);
    canvas_.DrawBatteryIcon(362, 30, battery_pct, charging, 3, 15);

    // Left card: fridge inventory
    paper_card(8, 58, 228, 206);
    canvas_.FillRoundRectR(16, 64, 92, 23, 11, 13);
    canvas_.DrawIcon16(UiIcon::kFridge, 22, 67, 2);
    canvas_.DrawText(42, 67, "冰箱清单", 1, 0);
    std::string summary = "共" + std::to_string(all_items.size()) + "样";
    if (exp_count > 0) summary += " · " + std::to_string(exp_count) + "临期";
    canvas_.DrawText(116, 67, summary, 1, 5, 114);

    if (all_items.empty()) {
        canvas_.DrawText(36, 156, "冰箱空空的，放点食材吧~", 1, 6);
    } else {
        const int show = std::min<int>(5, static_cast<int>(all_items.size()));
        for (int i = 0; i < show; ++i) {
            const auto& it = all_items[i];
            const int y = 92 + i * 34;
            if (i > 0) canvas_.DottedHLine(18, y - 2, 208, 3, 11);
            bool urgent = false;
            const std::string tag = FormatItemDateTag(it, today, &urgent);
            const UiIcon ico = PixelCanvas::IconForFood(food_catalog::GuessIcon(it.name, it.zone));
            canvas_.DrawIcon16(ico, 16, y + 7, urgent ? 0 : 6);
            canvas_.DrawText(38, y + 6, it.name, 1, urgent ? 0 : 3, 72);
            canvas_.DrawText(112, y + 6, it.qty, 1, 6, 42);

            const int tw = canvas_.MeasureText(tag, 1);
            const int tx = 228 - tw - 12;
            if (urgent) {
                canvas_.FillRoundRectR(tx, y + 4, tw + 12, 21, 10, 1);
                canvas_.DrawText(tx + 6, y + 6, tag, 1, 15);
            } else {
                canvas_.FillRoundRectR(tx, y + 4, tw + 12, 21, 10, 14);
                canvas_.DrawText(tx + 6, y + 6, tag, 1, 4);
            }
        }
    }

    // Right top card: tonight's recipe (index-card look)
    paper_card(246, 58, 146, 112);
    canvas_.DrawIcon16(UiIcon::kPot, 254, 64, 2);
    canvas_.DrawText(274, 64, "今晚吃啥", 1, 2);
    if (top_recipe != nullptr) {
        const std::string tm = std::to_string(top_recipe->recipe.time_min) + "分";
        const int tmw = canvas_.MeasureText(tm, 1) + 12;
        canvas_.FillRoundRectR(384 - tmw, 63, tmw, 20, 10, 13);
        canvas_.DrawText(390 - tmw, 64, tm, 1, 2);
        canvas_.DrawText(254, 88, top_recipe->recipe.title, 2, 0, 132);
        canvas_.FillRoundRectR(254, 122, 130, 22, 11, 14);
        canvas_.DrawIcon16(UiIcon::kHeart, 259, 125, 4);
        canvas_.DrawText(279, 124, "匹配 " + std::to_string(top_recipe->match_pct) + "%", 1, 1, 100);
        canvas_.DrawText(256, 148, "唤醒按上键看食谱", 1, 6, 130);
    } else {
        canvas_.DrawText(258, 108, "暂无食谱", 1, 6);
    }

    // Right bottom: shopping list as a taped sticky note
    canvas_.FillRect(249, 183, 146, 82, 7);           // shadow
    canvas_.FillRect(246, 180, 146, 82, 15);
    for (int i = 0; i < 14; ++i) {                     // folded corner
        canvas_.HLine(392 - 14 + i, 262 - 14 + i, 14 - i, 11);
    }
    canvas_.FillRect(298, 175, 42, 11, 12);            // washi tape
    canvas_.DrawIcon16(UiIcon::kBasket, 254, 190, 2);
    canvas_.DrawText(274, 190, "待买清单", 1, 2);
    int pending = 0;
    std::string line_a, line_b;
    for (const auto& sh : shopping) {
        if (sh.checked) continue;
        pending++;
        if (pending == 1) line_a = "· " + sh.name;
        else if (pending == 2) line_b = "· " + sh.name;
    }
    const std::string pc = std::to_string(pending) + "样";
    const int pcw = canvas_.MeasureText(pc, 1) + 12;
    canvas_.FillRoundRectR(384 - pcw, 189, pcw, 20, 10, 3);
    canvas_.DrawText(390 - pcw, 190, pc, 1, 15);
    canvas_.DrawText(256, 216, line_a.empty() ? "清单已清空~" : line_a, 1, 2, 120);
    if (!line_b.empty()) canvas_.DrawText(256, 238, line_b, 1, 5, 120);

    // Footer ribbon
    canvas_.FillRoundRectR(8, 272, 384, 24, 12, 15);
    canvas_.RoundRectR(8, 272, 384, 24, 12, 9, 1);
    canvas_.DrawIcon16(UiIcon::kMoon, 16, 276, 5);
    char foot[96];
    std::snprintf(foot, sizeof(foot), "省电小憩 · 按 OK 或下键唤醒 · 明早 %02d:00 刷新", wake_hour);
    canvas_.DrawText(38, 275, foot, 1, 4, 348);

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
// Page 1: Fridge Inventory (2x4 cards with round icon badges)
// ============================================================================
esp_err_t FridgeUi::RenderFridgePage(const UiTopBar& bar,
                                     int cnt_all, int cnt_fr, int cnt_fz,
                                     const std::vector<FridgeItem>& items,
                                     int selected_idx,
                                     bool force_full) {
    canvas_.SetMode4bpp(false);
    canvas_.Clear(15);
    DrawTopNav(FridgeTab::kFridge, bar);

    // Read-only summary line (no borders -> never looks like a button)
    canvas_.DrawIcon16(UiIcon::kFridge, 8, 34, 0);
    canvas_.DrawText(28, 34, "共 " + std::to_string(cnt_all) + " 样", 1, 0);
    canvas_.DrawIcon16(UiIcon::kLeaf, 104, 34, 0);
    canvas_.DrawText(124, 34, "冷藏 " + std::to_string(cnt_fr), 1, 0);
    canvas_.DrawIcon16(UiIcon::kSnow, 196, 34, 0);
    canvas_.DrawText(216, 34, "冷冻 " + std::to_string(cnt_fz), 1, 0);

    if (items.empty()) {
        canvas_.DrawMascotPixel(170, 66, 3, false);
        canvas_.DrawIcon16(UiIcon::kSpark, 236, 68, 0);
        canvas_.DrawTextCentered(140, "冰箱空空的，等你来填满~", 1, 0);
        canvas_.RoundRectR(46, 172, 308, 34, 17, 0, 1);
        canvas_.DrawIcon16(UiIcon::kMic, 60, 181, 0);
        canvas_.DrawText(82, 180, "按住 OK 说“放两盒牛奶”试试", 1, 0, 262);
        DrawKeyLegend("切页", "按住说", "刷新");
        return Flush1bpp(force_full);
    }

    const int32_t today = FridgeStore::TodayDayNumber();
    constexpr int kPerPage = 8;
    const int n = static_cast<int>(items.size());
    const int clamped = std::clamp(selected_idx, 0, n - 1);
    const int page = clamped / kPerPage;
    const int total_pages = (n + kPerPage - 1) / kPerPage;
    const int start = page * kPerPage;
    const int end = std::min(n, start + kPerPage);

    constexpr int kColX[2] = {8, 197};
    constexpr int kCardW = 182;
    constexpr int kCardH = 46;

    for (int i = start; i < end; ++i) {
        const auto& it = items[i];
        const int local = i - start;
        const int cx = kColX[local % 2];
        const int cy = 56 + (local / 2) * 52;
        const bool sel = (i == clamped);

        if (sel) {
            canvas_.FillRoundRectR(cx + 3, cy + 3, kCardW, kCardH, 8, 0);
            canvas_.FillRoundRectR(cx, cy, kCardW, kCardH, 8, 15);
            canvas_.RoundRectR(cx, cy, kCardW, kCardH, 8, 0, 3);
        } else {
            canvas_.FillPatternRoundRectR(cx + 2, cy + 2, kCardW, kCardH, 8, UiFill::kDots25);
            canvas_.FillRoundRectR(cx, cy, kCardW, kCardH, 8, 15);
            canvas_.RoundRectR(cx, cy, kCardW, kCardH, 8, 0, 1);
        }

        // Round icon badge (inverted when selected)
        const int bx = cx + 20, by = cy + 23;
        const UiIcon ico = PixelCanvas::IconForFood(food_catalog::GuessIcon(it.name, it.zone));
        if (sel) {
            canvas_.Circle(bx, by, 14, 0, true);
            canvas_.DrawIcon16(ico, bx - 8, by - 8, 15);
        } else {
            canvas_.Circle(bx, by, 14, 0, false);
            canvas_.DrawIcon16(ico, bx - 8, by - 8, 0);
        }

        bool urgent = false;
        const std::string tag = FormatItemDateTag(it, today, &urgent);
        const int tag_w = canvas_.MeasureText(tag, 1) + 12;
        const int tx = cx + kCardW - tag_w - 6;
        const int text_x = cx + 40;
        canvas_.DrawText(text_x, cy + 5, it.name, 1, 0, std::max(34, tx - text_x - 4));
        canvas_.DrawText(text_x, cy + 25, it.qty + " · " + it.zone, 1, 0, kCardW - 46);
        if (urgent) {
            DrawChip(tx, cy + 5, tag, true);
        } else {
            canvas_.DrawText(tx + 6, cy + 6, tag, 1, 0);
        }
    }

    // Right-edge scrollbar (only if > 1 page)
    if (total_pages > 1) {
        canvas_.RoundRectR(389, 56, 7, 202, 3, 0, 1);
        const int track_h = 198;
        const int thumb_h = std::max(28, track_h / total_pages);
        const int thumb_y = 58 + (page * (track_h - thumb_h)) / (total_pages - 1);
        canvas_.FillRoundRectR(391, thumb_y, 3, thumb_h, 1, 0);
    }

    const std::string pill = total_pages > 1
                                 ? (std::to_string(page + 1) + "/" + std::to_string(total_pages) + "页")
                                 : "";
    DrawKeyLegend("切页", "操作/按住说", "选菜", pill);
    return Flush1bpp(force_full);
}

// Streamlined 3-action Item Popup (UP = close immediately!)
esp_err_t FridgeUi::RenderItemPopup(const FridgeItem& item, int action_idx, bool has_local_recipe) {
    const int px = 44, py = 40, pw = 312, ph = 214;
    const UiIcon ico = PixelCanvas::IconForFood(food_catalog::GuessIcon(item.name, item.zone));
    DrawPopupFrame(px, py, pw, ph, ico, item.name);

    const int32_t today = FridgeStore::TodayDayNumber();
    bool urgent = false;
    const std::string tag = FormatItemDateTag(item, today, &urgent);
    canvas_.DrawText(px + 16, py + 46, item.qty + " · " + item.zone, 1, 0, pw - 120);
    const int tag_w = canvas_.MeasureText(tag, 1) + 12;
    DrawChip(px + pw - 16 - tag_w, py + 45, tag, urgent);

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
        canvas_.DrawMascotPixel(170, 64, 3, false);
        canvas_.DrawTextCentered(140, "还没有食谱哦~", 1, 0);
        canvas_.RoundRectR(46, 172, 308, 34, 17, 0, 1);
        canvas_.DrawIcon16(UiIcon::kMic, 60, 181, 0);
        canvas_.DrawText(82, 180, "按住 OK 说“推荐一道快手菜”", 1, 0, 262);
        DrawKeyLegend("切页", "按住说", "刷新");
        return Flush1bpp(force_full);
    }

    const int r_i = std::clamp(recipe_idx, 0, static_cast<int>(recipes.size()) - 1);
    const auto& mv = recipes[r_i];

    // Title row: dish name + match chip
    const std::string match_s = "匹配 " + std::to_string(mv.match_pct) + "%";
    const int mw = canvas_.MeasureText(match_s, 1) + 30;
    canvas_.DrawText(12, 36, mv.recipe.title, 2, 0, 388 - mw - 24);
    canvas_.FillRoundRectR(388 - mw, 40, mw, 22, 11, 0);
    canvas_.DrawIcon16(UiIcon::kHeart, 394 - mw, 43, 15);
    canvas_.DrawText(388 - mw + 24, 42, match_s, 1, 15);

    canvas_.DrawClockIcon(13, 74, 0);
    canvas_.DrawText(27, 70, std::to_string(mv.recipe.time_min) + "分钟 · " + mv.recipe.summary,
                     1, 0, 362);

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
                               ? ("缺料(" + std::to_string(mv.missing_count) + ")加采购")
                               : "缺料已齐";
    const std::string btns[3] = {"开始做菜", b1, "换一道"};
    const UiIcon icons[3] = {UiIcon::kPot, UiIcon::kBasket, UiIcon::kRefresh};
    constexpr int bx[3] = {8, 136, 292};
    constexpr int bw[3] = {122, 150, 100};
    for (int i = 0; i < 3; ++i) {
        DrawButton(bx[i], 212, bw[i], 42, btns[i], icons[i], true, i == btn_idx);
    }

    DrawKeyLegend("切页", "确认/按住说", "换按钮",
                  std::to_string(r_i + 1) + "/" + std::to_string(recipes.size()) + "道");
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
        // Live countdown ticks in the top-right pill; this line only shows static state
        bool any_ring = false, any_act = false;
        for (const auto& t : timers) {
            if (t.ringing) any_ring = true;
            if (t.active) any_act = true;
        }
        std::string ts;
        if (any_ring) ts = "计时到!";
        else if (any_act) ts = "计时进行中";
        else if (step_timer_min > 0) ts = "建议" + std::to_string(step_timer_min) + "分钟";
        if (!ts.empty()) {
            const int tsw = canvas_.MeasureText(ts, 1);
            canvas_.DrawText(392 - tsw, 35, ts, 1, 0);
        }

        canvas_.DottedHLine(10, 60, 380, 3, 0);
        canvas_.DrawWrappedText(14, 68, 372, 4, 32, st.text, 2, 0);

        // Step progress dots
        const int dot_gap = 16;
        const int dots_w = (total_steps - 1) * dot_gap;
        int dx = (PixelCanvas::kWidth - dots_w) / 2;
        for (int i = 0; i < total_steps; ++i, dx += dot_gap) {
            if (i < clamped) canvas_.Circle(dx, 199, 4, 0, true);
            else if (i == clamped) { canvas_.Circle(dx, 199, 6, 0, false); canvas_.Circle(dx, 199, 3, 0, true); }
            else canvas_.Circle(dx, 199, 4, 0, false);
        }
    } else {
        canvas_.DrawMascotPixel(170, 52, 3, false);
        canvas_.DrawTextCentered(126, "先按上键去“食谱”选一道菜吧~", 1, 0);
    }

    std::string btn_text;
    UiIcon btn_icon = UiIcon::kClock;
    if (is_last_step && has_recipe) {
        btn_text = "做好啦! 自动扣减冰箱库存";
        btn_icon = UiIcon::kCheck;
    } else if (step_timer_min > 0) {
        btn_text = "启动本步计时 (" + std::to_string(step_timer_min) + " 分钟)";
    } else {
        btn_text = "启动快捷计时 (5 分钟)";
    }
    DrawButton(14, 212, 372, 44, btn_text, btn_icon, true, true);

    DrawKeyLegend("切页", is_last_step ? "完工/按住说" : "计时/按住说", "下一步");
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
                                       bool force_full) {
    canvas_.SetMode4bpp(false);
    canvas_.Clear(15);
    DrawTopNav(FridgeTab::kShopping, bar);

    int pending = 0, checked = 0;
    for (const auto& s : list) { if (s.checked) checked++; else pending++; }

    canvas_.DrawIcon16(UiIcon::kBasket, 8, 34, 0);
    canvas_.DrawText(28, 34, "逛超市清单", 1, 0);
    const std::string cnt = "待买 " + std::to_string(pending) + " · 已勾 " + std::to_string(checked);
    canvas_.DrawText(392 - canvas_.MeasureText(cnt, 1), 34, cnt, 1, 0);

    if (list.empty()) {
        canvas_.DrawMascotPixel(170, 64, 3, false);
        canvas_.DrawTextCentered(140, "采购清单空空的~", 1, 0);
        canvas_.RoundRectR(46, 172, 308, 34, 17, 0, 1);
        canvas_.DrawIcon16(UiIcon::kMic, 60, 181, 0);
        canvas_.DrawText(82, 180, "按住 OK 说“买牛奶”就能添加", 1, 0, 262);
        DrawKeyLegend("切页", "按住说", "刷新");
        return Flush1bpp(force_full);
    }

    const int n = static_cast<int>(list.size());
    const int total_sel = n + 1;
    const int clamped = std::clamp(selected_idx, 0, total_sel - 1);

    constexpr int kMaxVis = 4;
    constexpr int kRowH = 36;
    const int vis_start = (clamped < n && clamped >= kMaxVis) ? (clamped - kMaxVis + 1) : 0;
    const int vis_end = std::min(n, vis_start + kMaxVis);

    // Notepad lines
    for (int r = 0; r < kMaxVis; ++r) {
        canvas_.DottedHLine(10, 58 + (r + 1) * kRowH - 3, 380, 2, 0);
    }

    for (int i = vis_start; i < vis_end; ++i) {
        const auto& s = list[i];
        const int ry = 58 + (i - vis_start) * kRowH;
        const bool sel = (i == clamped);
        const uint8_t fg = sel ? 15 : 0;

        if (sel) {
            canvas_.FillRoundRectR(8, ry, 384, 30, 8, 0);
            canvas_.DrawPointer(14, ry + 11, 15);
        }
        const int bx = sel ? 28 : 16;
        if (s.checked) {
            canvas_.FillRoundRectR(bx, ry + 7, 16, 16, 4, fg);
            canvas_.DrawCheck(bx + 4, ry + 10, sel ? 0 : 15);
        } else {
            canvas_.RoundRectR(bx, ry + 7, 16, 16, 4, fg, 2);
        }

        canvas_.DrawText(bx + 24, ry + 7, s.name + " " + s.qty, 1, fg, 196);
        if (s.checked) {
            const int nw = std::min(196, canvas_.MeasureText(s.name + " " + s.qty, 1));
            canvas_.HLine(bx + 22, ry + 16, nw + 4, fg);  // crossed off
        }
        const std::string right_s = s.checked ? "已买" : s.source;
        if (!right_s.empty()) {
            const int rw = std::min(130, canvas_.MeasureText(right_s, 1));
            canvas_.DrawText(382 - rw, ry + 7, right_s, 1, fg, 130);
        }
    }
    if (vis_end < n) {
        for (int i = 0; i < 5; ++i) canvas_.HLine(380 + i, 198 + i, 9 - 2 * i, 0);  // more below
    }

    const bool sel_commit = (clamped == n);
    DrawButton(8, 212, 384, 44, "已勾选 (" + std::to_string(checked) + ") 一键放进冰箱",
               UiIcon::kFridge, true, sel_commit);

    DrawKeyLegend("切页", sel_commit ? "入库/按住说" : "勾选/按住说", "下一项");
    return Flush1bpp(force_full);
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

    // Left: QR in a rounded "scan me" frame (read-only)
    canvas_.RoundRectR(8, 36, 148, 154, 10, 0, 1);
    const int cap_w = std::min(132, canvas_.MeasureText(qr_caption, 1));
    canvas_.FillRect(82 - cap_w / 2 - 6, 30, cap_w + 12, 14, 15);
    canvas_.DrawText(82 - cap_w / 2, 30, qr_caption, 1, 0, 132);
    canvas_.DrawQrCode(20, 52, 124, qr_payload);
    canvas_.DrawText(12, 196, bar.ap_mode ? "ZecTrix-Fridge" : ("IP:" + ip), 1, 0, 146);
    canvas_.DrawText(12, 217, bar.ap_mode ? ("密码:" + ap_password) : std::string("手机扫码管理冰箱"), 1, 0, 146);
    canvas_.DrawText(12, 238, bar.ap_mode ? "连后开192.168.4.1" : "可配Wi-Fi与云端AI", 1, 0, 146);

    // Right: read-only status lines with icons
    canvas_.DrawIcon16(UiIcon::kWifi, 168, 36, 0);
    canvas_.DrawText(188, 36,
                     bar.ap_mode ? "AP配网热点中"
                                 : ((cfg.wifi_ssid.empty() ? std::string("未配置") : cfg.wifi_ssid) +
                                    (bar.wifi_ok ? " (已连)" : " (断开)")),
                     1, 0, 204);
    const bool is_pub_xz = cfg.ota_url.empty() ||
                           cfg.ota_url.find("tenclass.net") != std::string::npos;
    const bool is_lan_xz = cfg.ota_url.find("192.168.") != std::string::npos ||
                           cfg.ota_url.find("10.") != std::string::npos ||
                           cfg.ota_url.find("172.") != std::string::npos;
    const std::string ai_s = is_pub_xz ? "小智官方 (免Key)"
                                       : (is_lan_xz ? "局域网小智 (MCP)" : "自定义小智服务");
    canvas_.DrawIcon16(UiIcon::kMic, 168, 57, 0);
    canvas_.DrawText(188, 57, ai_s, 1, 0, 204);
    char bbuf[64];
    std::snprintf(bbuf, sizeof(bbuf), "%u%% · %s", pwr.battery_percent,
                  pwr.charge.power_present ? "USB供电" : "电池省电");
    canvas_.DrawBatteryIcon(167, 81, pwr.battery_percent, pwr.charge.charging, 0, 15);
    canvas_.DrawText(188, 78, bbuf, 1, 0, 204);
    canvas_.DottedHLine(168, 102, 222, 3, 0);

    const std::string ctrls[4] = {
        "语音说明 / 绑定码",
        bar.ap_mode ? "重连家庭Wi-Fi" : "开启AP配网热点",
        "立即小憩 (灰阶封面)",
        "清屏并关机",
    };
    const UiIcon icons[4] = {UiIcon::kMic, UiIcon::kWifi, UiIcon::kMoon, UiIcon::kPower};
    const int clamped = std::clamp(selected_idx, 0, 3);
    for (int i = 0; i < 4; ++i) {
        DrawButton(166, 110 + i * 38, 226, 32, ctrls[i], icons[i], true, i == clamped);
    }

    DrawKeyLegend("切页", "执行/按住说", "下一项");
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

    // Big 29px code on a dotted "ticket"
    canvas_.FillPatternRoundRectR(px + 66, py + 98, pw - 128, 52, 10, UiFill::kDots25);
    canvas_.FillRoundRectR(px + 62, py + 94, pw - 128, 52, 10, 15);
    canvas_.RoundRectR(px + 62, py + 94, pw - 128, 52, 10, 0, 2);
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

esp_err_t FridgeUi::RenderSleepTimerConfirmPopup(const std::string& timer_desc, int choice_idx) {
    const int px = 34, py = 42, pw = 332, ph = 212;
    DrawPopupFrame(px, py, pw, ph, UiIcon::kClock, "厨房倒计时运行中!", Mood::kWorried);

    canvas_.DrawWrappedText(
        px + 16, py + 46, pw - 32, 2, 21,
        "正在计时（" + timer_desc + "），休眠或关机后将无法到点响铃。",
        1, 0);

    const char* opts[2] = {"继续做菜，暂不休眠", "取消全部计时并休眠"};
    const UiIcon icons[2] = {UiIcon::kPot, UiIcon::kMoon};
    for (int i = 0; i < 2; ++i) {
        DrawButton(px + 16, py + 96 + i * 38, pw - 32, 31, opts[i], icons[i], true, i == choice_idx);
    }

    canvas_.DottedHLine(px + 12, py + ph - 33, pw - 24, 3, 0);
    DrawKeyHints(px + 16, py + ph - 26, {{"上", "返回"}, {"OK", "确认"}, {"下", "换选项"}});
    return FlushRect1bpp({px - 6, py - 6, pw + 16, ph + 16});
}

esp_err_t FridgeUi::ShowAlarmScreen(const std::vector<KitchenTimer>& timers) {
    canvas_.SetMode4bpp(false);
    canvas_.Clear(15);
    // Tablecloth frame
    canvas_.FillPattern(0, 0, PixelCanvas::kWidth, PixelCanvas::kHeight, UiFill::kGingham);
    canvas_.FillRoundRectR(18, 16, 364, 268, 14, 15);
    canvas_.RoundRectR(18, 16, 364, 268, 14, 0, 3);

    canvas_.DrawMascotPixel(170, 30, 3, true);
    canvas_.DrawIcon16(UiIcon::kBell, 130, 52, 0);
    canvas_.DrawIcon16(UiIcon::kBell, 254, 52, 0);
    canvas_.DrawTextCentered(98, "时间到啦!", 2, 0);

    std::string label = "厨房倒计时已结束";
    for (const auto& t : timers) {
        if (t.ringing) { label = t.label + " 好啦!"; break; }
    }
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
