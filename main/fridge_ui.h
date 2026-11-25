#ifndef ZECTRIX_FRIDGE_UI_H_
#define ZECTRIX_FRIDGE_UI_H_

#include <string>
#include <vector>

#include "esp_err.h"
#include "fridge_store.h"
#include "pixel_canvas.h"
#include "zectrix_board.h"
#include "zectrix_epd.h"

enum class FridgeTab : int {
    kFridge = 0,
    kRecipe = 1,
    kCook = 2,
    kShopping = 3,
    kSettings = 4,
    kCount = 5,
};

enum class UiTheme : int {
    kMinimal = 0,  // MUJI / Apple / Kindle minimalist luxury
    kCard = 1,     // Warm kitchen rounded card stationery
};

struct UiTopBar {
    std::string hm_time;
    bool wifi_ok = false;
    bool ap_mode = false;
    int battery_pct = 100;
    bool charging = false;
    std::string active_timer_label;  // e.g. "剩2分" or "00:40"
    bool nav_focus = false;  // true = top menu bar has focus (UP/DOWN pick page, OK enters)
};

class FridgeUi {
public:
    explicit FridgeUi(zectrix_epd_handle_t epd = nullptr);
    void SetEpd(zectrix_epd_handle_t epd) { epd_ = epd; }
    void SetTheme(UiTheme theme) { theme_ = theme; }
    void SetTheme(int theme_id) { theme_ = (theme_id == 1) ? UiTheme::kCard : UiTheme::kMinimal; }
    UiTheme theme() const { return theme_; }
    bool is_minimal() const { return theme_ == UiTheme::kMinimal; }

    esp_err_t ClearDisplay();
    esp_err_t ShowBootSplash(const std::string& subtitle);

    // Page 0: True 4bpp 16-level grayscale standby poster
    //   0 = MUJI 瑞士双栏, 1 = Apple 极简大时钟, 2 = Kindle 日历书页,
    //   3 = 原研哉 黑白撞色, 4 = 温馨圆角卡片看板
    esp_err_t RenderStandby4bpp(const std::string& date_str,
                                const std::string& hm_str,
                                int battery_pct, bool charging,
                                const std::vector<FridgeItem>& all_items,
                                const RecipeMatchView* top_recipe,
                                const std::vector<ShoppingItem>& shopping,
                                int wake_hour,
                                int standby_style = 0);

    // Page 1: Fridge Inventory (Master-Detail Hero Focus Card + 5 Right Slim Cards)
    static constexpr int kFridgePageSize = 5;
    esp_err_t RenderFridgePage(const UiTopBar& bar,
                               int cnt_all, int cnt_fr, int cnt_fz,
                               const std::vector<FridgeItem>& items,
                               int selected_idx,
                               bool force_full);

    // Streamlined 3-action popup for selected fridge item (UP = close immediately)
    esp_err_t RenderItemPopup(const FridgeItem& item, int action_idx, bool has_local_recipe = true);

    // Non-blocking progress card while AI generates custom recipe
    esp_err_t RenderLoadingPopup(const std::string& title, const std::string& detail);

    // Confirmation dialog when user requests sleep/reboot/shutdown while kitchen timer is active
    esp_err_t RenderSleepTimerConfirmPopup(const std::string& timer_desc, int choice_idx,
                                           const char* action_label = "休眠");

    // Power action picker opened from Page 5 ("重启 / 清屏关机")
    esp_err_t RenderPowerMenuPopup(int choice_idx);

    // Page 2: Interlocking Recipe (read-only info borderless; 3 unmistakable buttons at bottom)
    esp_err_t RenderRecipePage(const UiTopBar& bar,
                               const std::vector<RecipeMatchView>& recipes,
                               int recipe_idx, int btn_idx,
                               bool force_full);

    // Page 3: 29px Large-Print Cooking Guide (borderless step & status; ONE unmistakable button)
    esp_err_t RenderCookPage(const UiTopBar& bar,
                             const RecipeMatchView* recipe,
                             int step_idx,
                             const std::vector<KitchenTimer>& timers,
                             bool force_full);

    esp_err_t RenderCookDonePopup(const RecipeMatchView& recipe, int choice_idx);

    // Page 4: Shopping List (selectable rows + commit & takeaway buttons)
    esp_err_t RenderShoppingPage(const UiTopBar& bar,
                                 const std::vector<ShoppingItem>& list,
                                 int selected_idx,
                                 bool force_full,
                                 bool has_nfc = true);

    // Takeaway QR modal from Page 4 (fallback for non-NFC batches & offline WeChat scan)
    esp_err_t RenderShoppingQrPopup(const std::string& qr_payload,
                                    bool online_mode,
                                    bool has_nfc,
                                    const std::string& ip,
                                    int pending_count);

    // Page 5: Settings & QR (borderless QR/info; 4 unmistakable buttons, NO NFC)
    esp_err_t RenderSettingsPage(const UiTopBar& bar,
                                 const AppConfig& cfg,
                                 const std::string& ip,
                                 const std::string& qr_payload,
                                 const std::string& qr_caption,
                                 const std::string& ap_password,
                                 const ZectrixPowerSnapshot& pwr,
                                 int selected_idx,
                                 bool force_full);

    // Crystal-clear center modal for voice recognition result (never a mystery beep!)
    esp_err_t RenderVoiceResultPopup(bool success,
                                     const std::string& heard,
                                     const std::string& action_detail,
                                     bool can_undo);

    // Built-in spoken command cheat sheet (accessible from Page 5)
    esp_err_t RenderVoiceHelpPopup();

    // Big 29px 6-digit bind code modal for xiaozhi.me zero-key activation!
    esp_err_t RenderActivationPopup(const std::string& code, const std::string& status_line);

    // Lightweight timer-pill partial refresh ONLY (never increments full-screen flash counter!)
    esp_err_t UpdateTimerPillOnly(const UiTopBar& bar);

    esp_err_t ShowQaPopup(const std::string& title, const std::string& answer);
    esp_err_t ShowAlarmScreen(const std::vector<KitchenTimer>& timers);

private:
    struct KeyHint {
        const char* key;
        std::string desc;
    };
    enum class Mood : uint8_t { kNone = 0, kHappy, kWorried };

    void DrawTopNav(FridgeTab active, const UiTopBar& bar);
    // Right-hand status area (timer pill / Wi-Fi + clock / battery), shared with pill-only refresh
    void DrawStatusArea(const UiTopBar& bar);
    // Warm popup card: tablecloth header band + white title tag (+ optional peeking mascot).
    // Clears its own footprint; returns nothing — content starts at py + 46.
    void DrawPopupFrame(int px, int py, int pw, int ph, UiIcon icon,
                        const std::string& title, Mood mood = Mood::kNone);
    // Inline row of black keycaps + labels (used by bottom legend and popup footers)
    int DrawKeyHints(int x, int y, const std::vector<KeyHint>& hints);
    // Selectable rounded button with optional icon (selected = solid black + pointer)
    void DrawButton(int x, int y, int w, int h, const std::string& label,
                    UiIcon icon, bool has_icon, bool selected);
    // Small rounded status chip; returns its width
    int DrawChip(int x, int y, const std::string& text, bool solid);
    // Bottom legend: [上/下] <nav_label>  [OK] <ok_label>  [长按上] 切页
    void DrawKeyLegend(const std::string& nav_label, const std::string& ok_label);
    esp_err_t Flush1bpp(bool force_full);
    esp_err_t FlushRect1bpp(const zectrix_epd_rect_t& rect, bool count_for_full = true);

    zectrix_epd_handle_t epd_ = nullptr;
    PixelCanvas canvas_;
    bool has_1bpp_base_ = false;
    int partial_count_ = 0;
    int pill_partial_count_ = 0;
    bool nav_focus_ = false;  // latched by DrawTopNav for the legend of the same frame
    UiTheme theme_ = UiTheme::kMinimal;
};

#endif  // ZECTRIX_FRIDGE_UI_H_
