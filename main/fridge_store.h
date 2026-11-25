#ifndef ZECTRIX_FRIDGE_STORE_H_
#define ZECTRIX_FRIDGE_STORE_H_

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include "esp_err.h"
#include "food_catalog.h"

// Optional git-ignored developer defaults (see main/local_secrets.h.example)
#if __has_include("local_secrets.h")
#include "local_secrets.h"
#endif
#ifndef FRIDGE_DEFAULT_WIFI_SSID
#define FRIDGE_DEFAULT_WIFI_SSID ""
#endif
#ifndef FRIDGE_DEFAULT_WIFI_PASS
#define FRIDGE_DEFAULT_WIFI_PASS ""
#endif
#ifndef FRIDGE_DEFAULT_OTA_URL
#define FRIDGE_DEFAULT_OTA_URL "https://api.tenclass.net/xiaozhi/ota/"
#endif
#ifndef FRIDGE_DEFAULT_NFC_SHOP_URL
#define FRIDGE_DEFAULT_NFC_SHOP_URL ""
#endif

enum class ZoneFilter : int {
    kAll = 0,
    kExpiring = 1,
    kFridge = 2,
    kFreezer = 3,
    kCount = 4,
};

struct FridgeItem {
    uint32_t id = 0;
    std::string name;
    std::string qty = "1份";
    std::string zone = "冷藏";  // 冷藏 / 冷冻 / 常温
    int32_t added_day = 0;
    int32_t expire_day = 0;
    std::string note;
    bool storage_reminded = false;  // "已经放了N天啦" already spoken once for this item
};

struct ShoppingItem {
    uint32_t id = 0;
    std::string name;
    std::string qty = "1份";
    std::string source;  // e.g. "番茄滑蛋缺料" / "冰箱补货" / "语音添加"
    bool checked = false;
};

struct KitchenTimer {
    uint32_t id = 0;
    std::string label;
    int32_t total_sec = 0;
    int64_t end_uptime_ms = 0;
    bool active = false;
    bool ringing = false;
};

struct RecipeIngredient {
    std::string name;
    std::string amount;
};

struct RecipeStep {
    std::string text;
    int timer_min = 0;      // >0 enables one-click step timer on Page 3!
    std::string heat;       // e.g. "中小火" / "大火" / "备菜"
};

struct RecipeCard {
    std::string title;
    std::string summary;
    int time_min = 15;
    std::vector<RecipeIngredient> ingredients;
    std::vector<RecipeStep> steps;
    bool custom = false;  // true = AI/user-pushed recipe (stays pinned ahead of starter dishes!)
};

enum class MatchState : uint8_t {
    kPantry = 0,      // 家常常备油盐
    kHaveUrgent = 1,  // 冰箱已有且临期 (<=3天)
    kHaveFresh = 2,   // 冰箱已有
    kMissing = 3,     // 冰箱缺料
};

struct MatchedIngredient {
    std::string name;
    std::string amount;
    MatchState state = MatchState::kMissing;
    uint32_t fridge_item_id = 0;
    std::string fridge_qty;
    int days_left = 999;
};

struct RecipeMatchView {
    RecipeCard recipe;
    std::vector<MatchedIngredient> matched;
    int have_count = 0;
    int missing_count = 0;
    int urgent_hits = 0;
    int match_pct = 0;
};

struct AiLogEntry {
    std::string time_str;
    std::string input;
    std::string output;
    int latency_ms = 0;
    bool ok = false;
};

struct AppConfig {
    std::string wifi_ssid = FRIDGE_DEFAULT_WIFI_SSID;
    std::string wifi_pass = FRIDGE_DEFAULT_WIFI_PASS;
    // Xiaozhi OTA URL (default: public free xiaozhi.me) OR private NAS OTA/WS URL
    // e.g. "https://api.tenclass.net/xiaozhi/ota/" or "http://192.168.1.100:8003/xiaozhi/ota/"
    std::string ota_url = FRIDGE_DEFAULT_OTA_URL;
    // Optional custom Bearer token (used when connecting directly to a custom ws:// / wss:// URL)
    std::string ws_token;
    // Static offline shopping list page URL written into the 880B NFC tag (#<base64url> appended)
    std::string nfc_shop_url = FRIDGE_DEFAULT_NFC_SHOP_URL;
    int sleep_minutes = 5;            // 0 = never auto-sleep on battery
    int wake_hour = 8;                // Daily RTC morning refresh hour
    int standby_style = 0;            // 0 = 温馨手账看板, 1 = 极简大时钟桌搭
};

struct LocalFastAction {
    bool handled = false;
    std::string receipt;
    bool switch_to_cook = false;
    int step_delta = 0;  // +1 next step, -1 prev step
    bool stopped_timers = false;
};

class FridgeStore {
public:
    static constexpr size_t kMaxItems = 80;
    static constexpr size_t kMaxShopping = 80;
    static constexpr size_t kMaxTimers = 3;
    static constexpr size_t kMaxRecipes = 5;
    static constexpr size_t kMaxAiLogs = 5;

    esp_err_t Init();

    static bool IsClockSynced();
    static int32_t TodayDayNumber();
    static std::string FormatDate(int32_t day_num);
    static std::string NowHmString();
    static int ComputeItemAgeDays(const FridgeItem& it, int32_t today);
    static int ComputeItemDaysLeft(const FridgeItem& it, int32_t today, bool* has_expiry = nullptr);

    // Stamp any items added while clock was unsynced once RTC/SNTP provides valid date
    void ReconcileUnsyncedDates();

    // Config (NVS)
    AppConfig GetConfig();
    esp_err_t SaveConfig(const AppConfig& cfg);

    // Fridge inventory (Page 1)
    std::vector<FridgeItem> GetItemsFiltered(ZoneFilter filter,
                                             int* total_all = nullptr,
                                             int* total_expiring = nullptr,
                                             int* total_fridge = nullptr,
                                             int* total_freezer = nullptr);
    uint32_t AddOrUpdateItem(const std::string& name, const std::string& qty,
                             const std::string& zone, int days_from_today,
                             const std::string& note = "");
    bool CanAddOrUpdateItem(const std::string& name);
    bool HasItemMatchingName(const std::string& name);
    bool RemoveItemById(uint32_t id);
    bool RemoveItemByName(const std::string& name);
    bool AdjustItemExpiry(uint32_t id, int delta_days);
    // Set explicit remaining shelf-life days by food name (preserves existing qty & zone).
    // Returns matched item name if found & updated, or empty string if not in fridge yet.
    std::string SetItemExpiryByName(const std::string& name, int days_from_today);
    bool DeductItemAmount(uint32_t id, const std::string& used_amount);
    bool DecrementItemQty(uint32_t id);
    bool MoveItemToShopping(uint32_t id);

    // Interlocking Recipe Engine (Page 2 & Page 3)
    bool HasRecipeForIngredient(const std::string& ingredient_name);
    std::vector<RecipeMatchView> GetMatchedRecipesSorted(const std::string& focus_ingredient = "");
    int AddMissingToShopping(const RecipeMatchView& view);
    int DeductCookedIngredients(const RecipeMatchView& view);
    void PushRecipe(const RecipeCard& recipe);
    bool RemoveRecipe(const std::string& title);

    // Shopping list (Page 4)
    std::vector<ShoppingItem> GetShoppingList();
    bool CanAddShoppingItem(const std::string& name);
    uint32_t AddShoppingItem(const std::string& name, const std::string& qty,
                             const std::string& source = "");
    bool ToggleShoppingItem(uint32_t id);
    bool RemoveShoppingItem(uint32_t id);
    int CommitCheckedShoppingToFridge(int* skipped_full_out = nullptr);
    // Commit all shopping items (or only items matching `filter_names` when non-empty) into fridge.
    int CommitAllShoppingToFridge(const std::vector<std::string>& filter_names = {},
                                  std::vector<std::string>* moved_names_out = nullptr,
                                  int* skipped_full_out = nullptr);

    // Kitchen timers (Page 3 & global top bar)
    std::vector<KitchenTimer> GetTimers();
    bool StartTimer(const std::string& label, int minutes, int seconds = 0);
    bool StopTimer(uint32_t id);
    bool StopAllRinging();
    bool StopAllTimers();
    bool TickTimers(int64_t now_uptime_ms, bool* any_ringing, bool* any_active);

    // Undo for voice commands
    void SaveUndoSnapshot();
    bool HasUndoSnapshot();
    bool UndoLastVoiceAction();

    // 0ms local fast-path for frequent kitchen voice commands (timers / steps)
    LocalFastAction TryLocalFastCommand(const std::string& text);

    // AI logs & prompt context
    void RecordAiLog(const std::string& input, const std::string& output,
                     int latency_ms, bool ok);
    std::string BuildPromptContext(const std::string& focus_ingredient = "");
    std::string ExportStateJson(const std::string& ip, bool wifi_ok, bool ap_mode,
                                int battery_pct, bool charging);

    bool ConsumeStorageWarning(std::string* msg_out);

    // Day number (TodayDayNumber) of the last spoken morning expiry reminder (NVS)
    int32_t GetLastReminderDay();
    void SetLastReminderDay(int32_t day);
    // Short spoken nudge such as "冻鸡腿已经放了270天啦" for items that reached their
    // recommended storage time (USDA chart; fridge vs freezer by storage zone) and have
    // not been mentioned before. Never phrased as "expired". ids_out = items named in it.
    std::string BuildStorageReminderText(std::vector<uint32_t>* ids_out);
    // Each item is reminded only once: call after the notice was actually spoken.
    void MarkStorageReminded(const std::vector<uint32_t>& ids);

private:
    bool TryLoadJsonFileLocked(const char* path, bool* file_existed);
    void LoadFromSpiffsLocked();
    void SaveToSpiffsLocked();
    void LoadConfigFromNvsLocked();
    void SeedDefaultRecipesLocked();
    void SeedStarterDataLocked();
    int FindBestItemIndexLocked(const std::string& query) const;
    RecipeMatchView MatchSingleLocked(const RecipeCard& r, int32_t today) const;

    std::mutex mutex_;
    AppConfig config_;
    std::vector<FridgeItem> items_;
    std::vector<ShoppingItem> shopping_;
    std::vector<RecipeCard> recipes_;
    std::vector<KitchenTimer> timers_;
    std::vector<AiLogEntry> ai_logs_;

    // Undo snapshot
    bool has_undo_ = false;
    std::vector<FridgeItem> undo_items_;
    std::vector<ShoppingItem> undo_shopping_;
    std::vector<RecipeCard> undo_recipes_;

    uint32_t next_id_ = 1;
    bool spiffs_ready_ = false;
    std::string storage_warning_;
};

#endif  // ZECTRIX_FRIDGE_STORE_H_
