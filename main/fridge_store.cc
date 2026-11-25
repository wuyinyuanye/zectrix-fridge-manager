#include "fridge_store.h"

#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

#include "cJSON.h"
#include "esp_log.h"
#include "esp_spiffs.h"
#include "esp_timer.h"
#include "nvs.h"
#include "nvs_flash.h"

namespace {

constexpr char kTag[] = "fridge_store";
constexpr char kDataPath[] = "/spiffs/fridge_v2.json";
constexpr char kTmpPath[] = "/spiffs/fridge_v2.tmp";
constexpr char kBakPath[] = "/spiffs/fridge_v2.bak";
constexpr char kCorruptPath[] = "/spiffs/fridge_v2.corrupt";
constexpr char kNvsNs[] = "fridge_v2";

// Sentinel base when explicit shelf-life days are recorded while system clock is unsynced:
// expire_day = kUnsyncedExpiryBase - clamped_days  (always <= -40000).
constexpr int32_t kUnsyncedExpiryBase = -100000;
constexpr int32_t kUnsyncedExpiryThreshold = -40000;

std::string ReadNvsStr(nvs_handle_t h, const char* k, const std::string& fb) {
    size_t len = 0;
    if (nvs_get_str(h, k, nullptr, &len) != ESP_OK || len == 0) return fb;
    std::string out(len - 1, '\0');
    if (nvs_get_str(h, k, out.data(), &len) != ESP_OK) return fb;
    return out;
}

std::string JStr(cJSON* o, const char* k, const char* fb = "") {
    cJSON* it = cJSON_GetObjectItemCaseSensitive(o, k);
    return (cJSON_IsString(it) && it->valuestring) ? it->valuestring : fb;
}

int JInt(cJSON* o, const char* k, int fb = 0) {
    cJSON* it = cJSON_GetObjectItemCaseSensitive(o, k);
    if (!cJSON_IsNumber(it)) return fb;
    const double d = it->valuedouble;
    if (d > 100000000.0) return 100000000;
    if (d < -100000000.0) return -100000000;
    return static_cast<int>(d);
}

bool JBool(cJSON* o, const char* k, bool fb = false) {
    cJSON* it = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsBool(it) ? cJSON_IsTrue(it) : fb;
}

std::string TrimSpaces(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r' || s[a] == '\n')) ++a;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r' || s[b - 1] == '\n')) --b;
    return s.substr(a, b - a);
}

// UTF-8 safe byte-length clamp so serialized SPIFFS JSON can never approach the 256KB ceiling
std::string ClampUtf8Bytes(const std::string& raw, size_t max_bytes) {
    std::string s = TrimSpaces(raw);
    if (s.size() <= max_bytes) return s;
    size_t cut = max_bytes;
    while (cut > 0 && (static_cast<uint8_t>(s[cut]) & 0xC0) == 0x80) {
        --cut;
    }
    return s.substr(0, cut);
}

// Map whatever the user / Xiaozhi said ("冷冻室", "冻起来", "保鲜层", "室温"...) onto the
// three zones the app understands. Empty = unknown -> caller falls back to the food hint.
std::string NormalizeZone(const std::string& raw) {
    const std::string z = TrimSpaces(raw);
    if (z.empty()) return "";
    auto has = [&](const char* k) { return z.find(k) != std::string::npos; };
    if (has("冷冻") || has("冰冻") || has("速冻") || has("冻") || z == "freezer") return "冷冻";
    if (has("常温") || has("室温") || has("阴凉") || has("外面") || has("柜") || z == "room") return "常温";
    if (has("冷藏") || has("保鲜") || has("冰箱") || has("藏") || z == "fridge") return "冷藏";
    return "";
}

constexpr size_t kMaxNameBytes = 64;
constexpr size_t kMaxQtyBytes = 32;
constexpr size_t kMaxZoneBytes = 24;
constexpr size_t kMaxNoteBytes = 128;
constexpr size_t kMaxStepTextBytes = 160;
constexpr size_t kMaxRecipeIngredients = 16;
constexpr size_t kMaxRecipeSteps = 12;

bool FileExists(const char* path) {
    struct stat st = {};
    return stat(path, &st) == 0;
}

// Guard against false substring collisions such as "牛奶" matching "酸牛奶"/"酸奶"/"牛奶糖"
bool HasConflictingModifier(const std::string& a, const std::string& b) {
    auto conflict = [&](const char* base, const char* other) {
        const bool a_other = a.find(other) != std::string::npos;
        const bool b_other = b.find(other) != std::string::npos;
        if (a_other != b_other &&
            (a.find(base) != std::string::npos || b.find(base) != std::string::npos)) {
            return true;
        }
        return false;
    };
    return conflict("牛奶", "酸牛奶") || conflict("牛奶", "酸奶") ||
           conflict("牛奶", "牛奶糖") || conflict("鸡蛋", "咸鸡蛋") ||
           conflict("鸡蛋", "皮蛋");
}

bool AreExactAliases(const std::string& a, const std::string& b) {
    static constexpr const char* kGroups[][4] = {
        {"番茄", "西红柿", "圣女果", nullptr},
        {"鸡蛋", "土鸡蛋", "鲜鸡蛋", nullptr},
        {"牛奶", "鲜牛奶", "纯牛奶", nullptr},
        {"菠菜", "嫩菠菜", nullptr, nullptr},
        {"虾仁", "鲜虾仁", "鲜虾", nullptr},
        {"土豆", "马铃薯", nullptr, nullptr},
        {"小葱", "香葱", "小香葱", nullptr},
    };
    for (const auto& grp : kGroups) {
        bool hit_a = false, hit_b = false;
        for (int i = 0; i < 4 && grp[i] != nullptr; ++i) {
            if (a == grp[i]) hit_a = true;
            if (b == grp[i]) hit_b = true;
        }
        if (hit_a && hit_b) return true;
    }
    return false;
}

bool IngredientMatchesFood(const std::string& ing_name, const std::string& food_name) {
    if (ing_name.empty() || food_name.empty()) return false;
    if (ing_name == food_name || AreExactAliases(ing_name, food_name)) return true;
    if (HasConflictingModifier(ing_name, food_name)) return false;
    if (food_name.find(ing_name) != std::string::npos ||
        ing_name.find(food_name) != std::string::npos) {
        return true;
    }
    auto pair_match = [&](const char* a, const char* b) {
        return (ing_name.find(a) != std::string::npos && food_name.find(b) != std::string::npos) ||
               (ing_name.find(b) != std::string::npos && food_name.find(a) != std::string::npos);
    };
    return pair_match("番茄", "西红柿") || pair_match("番茄", "圣女果") ||
           pair_match("西红柿", "圣女果") || pair_match("鸡蛋", "土鸡蛋") ||
           pair_match("菠菜", "嫩菠菜") || pair_match("虾仁", "鲜虾") ||
           pair_match("土豆", "马铃薯");
}

// ============================================================================
// Unit-aware Quantity Parser, Merger & Cooking Portion Deductor
// ============================================================================
enum class QtyKind : uint8_t { kUnknown = 0, kMass, kVolume, kCount };

struct ParsedQty {
    QtyKind kind = QtyKind::kUnknown;
    double value = 0.0;       // normalized g for kMass, ml for kVolume, count for kCount
    std::string raw_unit;
};

int ParseChineseIntPrefix(const std::string& s, size_t* consumed) {
    static constexpr struct { const char* tok; int val; } kDigits[] = {
        {"零", 0}, {"一", 1}, {"二", 2}, {"两", 2}, {"三", 3}, {"四", 4},
        {"五", 5}, {"六", 6}, {"七", 7}, {"八", 8}, {"九", 9},
    };
    size_t pos = 0;
    int total = 0;
    int cur_digit = -1;
    bool any = false;

    while (pos < s.size()) {
        if (s.compare(pos, 3, "百") == 0) {
            total += (cur_digit <= 0 ? 1 : cur_digit) * 100;
            cur_digit = -1;
            pos += 3;
            any = true;
            continue;
        }
        if (s.compare(pos, 3, "十") == 0) {
            total += (cur_digit <= 0 ? 1 : cur_digit) * 10;
            cur_digit = -1;
            pos += 3;
            any = true;
            continue;
        }
        bool matched = false;
        for (const auto& d : kDigits) {
            const size_t len = std::strlen(d.tok);
            if (s.compare(pos, len, d.tok) == 0) {
                cur_digit = d.val;
                pos += len;
                any = true;
                matched = true;
                break;
            }
        }
        if (!matched) break;
    }
    if (!any) {
        *consumed = 0;
        return 0;
    }
    if (cur_digit > 0) total += cur_digit;
    *consumed = pos;
    return total;
}

ParsedQty ParseQty(const std::string& raw) {
    ParsedQty out;
    const std::string s = TrimSpaces(raw);
    if (s.empty()) return out;

    size_t pos = 0;
    double num = 0.0;
    bool has_num = false;

    if (s[0] >= '0' && s[0] <= '9') {
        char* endp = nullptr;
        num = std::strtod(s.c_str(), &endp);
        if (endp && endp > s.c_str()) {
            pos = static_cast<size_t>(endp - s.c_str());
            has_num = true;
        }
    } else if (s.compare(0, 3, "半") == 0) {
        num = 0.5;
        pos = 3;
        has_num = true;
    } else {
        size_t cn_len = 0;
        const int cn = ParseChineseIntPrefix(s, &cn_len);
        if (cn_len > 0 && cn > 0) {
            num = static_cast<double>(cn);
            pos = cn_len;
            has_num = true;
        }
    }
    if (!has_num || num <= 0.0) return out;

    std::string u = TrimSpaces(s.substr(pos));
    if (u.compare(0, 3, "个") == 0 && u.compare(3, 3, "半") == 0) {
        num += 0.5;
        u = TrimSpaces(u.substr(6));
        if (u.empty()) u = "个";
    } else if (u.size() >= 6 && u.compare(3, 3, "半") == 0) {
        // e.g. "1斤半"
        num += 0.5;
        u = u.substr(0, 3);
    }

    if (u == "g" || u == "G" || u == "克") {
        out.kind = QtyKind::kMass; out.value = num; out.raw_unit = u;
    } else if (u == "两") {
        out.kind = QtyKind::kMass; out.value = num * 50.0; out.raw_unit = u;
    } else if (u == "斤") {
        out.kind = QtyKind::kMass; out.value = num * 500.0; out.raw_unit = u;
    } else if (u == "kg" || u == "KG" || u == "千克" || u == "公斤") {
        out.kind = QtyKind::kMass; out.value = num * 1000.0; out.raw_unit = u;
    } else if (u == "ml" || u == "mL" || u == "ML" || u == "毫升") {
        out.kind = QtyKind::kVolume; out.value = num; out.raw_unit = u;
    } else if (u == "l" || u == "L" || u == "升") {
        out.kind = QtyKind::kVolume; out.value = num * 1000.0; out.raw_unit = u;
    } else {
        out.kind = QtyKind::kCount;
        out.value = num;
        out.raw_unit = u.empty() ? "份" : u;
    }
    return out;
}

std::string FormatMassGrams(double grams, const std::string& pref_unit) {
    const int g = std::max(1, static_cast<int>(std::llround(grams)));
    if (pref_unit == "斤" && g % 500 == 0) {
        return std::to_string(g / 500) + "斤";
    }
    if ((pref_unit == "kg" || pref_unit == "KG" || pref_unit == "千克" || pref_unit == "公斤") &&
        g % 1000 == 0) {
        return std::to_string(g / 1000) + pref_unit;
    }
    return std::to_string(g) + (pref_unit == "克" ? "克" : "g");
}

std::string MergeQuantityStrings(const std::string& old_q, const std::string& add_q) {
    if (add_q.empty()) return old_q.empty() ? "1份" : old_q;
    if (old_q.empty()) return add_q;
    const ParsedQty a = ParseQty(old_q);
    const ParsedQty b = ParseQty(add_q);
    if (a.kind == QtyKind::kMass && b.kind == QtyKind::kMass) {
        return FormatMassGrams(std::min(999000.0, a.value + b.value), a.raw_unit);
    }
    if (a.kind == QtyKind::kVolume && b.kind == QtyKind::kVolume) {
        const int ml = std::max(1, static_cast<int>(std::llround(std::min(999000.0, a.value + b.value))));
        return std::to_string(ml) + (a.raw_unit == "毫升" ? "毫升" : "ml");
    }
    if (a.kind == QtyKind::kCount && b.kind == QtyKind::kCount &&
        (a.raw_unit == b.raw_unit || b.raw_unit == "份" || a.raw_unit == "份")) {
        const int n = std::max(1, static_cast<int>(std::llround(std::min(9999.0, a.value + b.value))));
        const std::string& u = (a.raw_unit != "份") ? a.raw_unit : b.raw_unit;
        return std::to_string(n) + u;
    }
    return add_q;
}

// ============================================================================
// Accurate Spoken Duration Parser for Local Fast-Path (hours / minutes / seconds)
// Rejects clock-time expressions like "提醒我3点关火"!
// ============================================================================
int ParseNumberBeforeAt(const std::string& text, size_t unit_pos, bool* has_half_out = nullptr) {
    if (has_half_out) *has_half_out = false;
    size_t end = unit_pos;
    // Strip optional "个" right before 小时/钟头 (e.g. "一个半小时" / "2个小时" / "半个小时")
    if (end >= 3 && text.compare(end - 3, 3, "个") == 0) {
        end -= 3;
    }
    bool seen_half = false;
    if (end >= 3 && text.compare(end - 3, 3, "半") == 0) {
        seen_half = true;
        if (has_half_out) *has_half_out = true;
        end -= 3;
        if (end >= 3 && text.compare(end - 3, 3, "个") == 0) {
            end -= 3;
        }
        if (end == 0) return 0;  // pure "半小时"
    }

    // Try ASCII digits right before `end`
    size_t d_start = end;
    while (d_start > 0 && text[d_start - 1] >= '0' && text[d_start - 1] <= '9') {
        --d_start;
    }
    if (d_start < end) {
        int val = 0;
        for (size_t i = d_start; i < end; ++i) {
            val = std::min(10000, val * 10 + (text[i] - '0'));
        }
        return val;
    }

    // Walk backward across UTF-8 Chinese numeral characters
    static constexpr const char* kNumChars[] = {
        "零", "一", "二", "两", "三", "四", "五", "六", "七", "八", "九", "十", "百",
    };
    size_t c_start = end;
    while (c_start >= 3) {
        bool is_cn = false;
        for (const char* tok : kNumChars) {
            if (text.compare(c_start - 3, 3, tok) == 0) {
                is_cn = true;
                break;
            }
        }
        if (!is_cn) break;
        c_start -= 3;
    }
    if (c_start < end) {
        size_t consumed = 0;
        return ParseChineseIntPrefix(text.substr(c_start, end - c_start), &consumed);
    }
    // For prefixed forms like "定时半小时" or "定时半分钟", no number precedes "半" -> treat as 0 + half!
    return seen_half ? 0 : -1;
}

int ParseSpokenDurationSeconds(const std::string& text) {
    // Never mistake wall-clock times ("提醒我3点关火", "8:30") for countdown durations!
    if (text.find("点") != std::string::npos ||
        text.find(':') != std::string::npos ||
        text.find("：") != std::string::npos) {
        return 0;
    }

    int64_t total_sec = 0;
    bool matched_any = false;

    // 1. Hours: 小时 / 钟头
    size_t hr_pos = text.find("小时");
    size_t hr_len = 6;
    if (hr_pos == std::string::npos) {
        hr_pos = text.find("钟头");
        hr_len = 6;
    }
    size_t after_hr = 0;
    if (hr_pos != std::string::npos) {
        bool half = false;
        const int hrs = ParseNumberBeforeAt(text, hr_pos, &half);
        if (hrs >= 0 && (hrs > 0 || half)) {
            total_sec += static_cast<int64_t>(hrs) * 3600 + (half ? 1800 : 0);
            after_hr = hr_pos + hr_len;
            if (text.compare(after_hr, 3, "半") == 0) {
                total_sec += 1800;
                after_hr += 3;
            }
            matched_any = true;
        }
    }

    // 2. Minutes: 分钟 / 分半 / 分
    const std::string rem = text.substr(after_hr);
    size_t min_pos = rem.find("分钟");
    size_t min_len = 6;
    bool trailing_half_min = false;
    if (min_pos == std::string::npos) {
        min_pos = rem.find("分半");
        if (min_pos != std::string::npos) {
            min_len = 6;
            trailing_half_min = true;
        } else {
            size_t search_from = 0;
            while ((min_pos = rem.find("分", search_from)) != std::string::npos) {
                // Skip Chinese fraction expressions like "三分之一" / "百分之五十"!
                if (min_pos + 6 <= rem.size() && rem.compare(min_pos, 6, "分之") == 0) {
                    search_from = min_pos + 6;
                    continue;
                }
                break;
            }
            min_len = 3;
        }
    }
    size_t after_min = 0;
    if (min_pos != std::string::npos) {
        bool half = false;
        const int mins = ParseNumberBeforeAt(rem, min_pos, &half);
        if (mins >= 0 && (mins > 0 || half)) {
            total_sec += static_cast<int64_t>(mins) * 60 + ((half || trailing_half_min) ? 30 : 0);
            after_min = min_pos + min_len;
            matched_any = true;
        }
    }

    // 3. Seconds: 秒
    const std::string rem_sec = rem.substr(after_min);
    const size_t sec_pos = rem_sec.find("秒");
    if (sec_pos != std::string::npos) {
        const int secs = ParseNumberBeforeAt(rem_sec, sec_pos, nullptr);
        if (secs > 0) {
            total_sec += secs;
            matched_any = true;
        }
    }

    if (!matched_any || total_sec <= 0 || total_sec > 86400) return 0;
    return static_cast<int>(total_sec);
}

}  // namespace

bool FridgeStore::IsClockSynced() {
    const time_t now = std::time(nullptr);
    struct tm lt = {};
    localtime_r(&now, &lt);
    return (lt.tm_year + 1900) >= 2025;
}

int32_t FridgeStore::TodayDayNumber() {
    const time_t now = std::time(nullptr);
    struct tm lt = {};
    localtime_r(&now, &lt);
    if (lt.tm_year + 1900 < 2025) return 0;  // 0 = clock not synced yet (never fabricate a date!)
    lt.tm_hour = 12;
    lt.tm_min = 0;
    lt.tm_sec = 0;
    return static_cast<int32_t>(mktime(&lt) / 86400);
}

std::string FridgeStore::FormatDate(int32_t day_num) {
    if (day_num <= 0) return "未校时";
    const time_t t = static_cast<time_t>(day_num) * 86400 + 43200;
    struct tm lt = {};
    localtime_r(&t, &lt);
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%02d/%02d", lt.tm_mon + 1, lt.tm_mday);
    return buf;
}

std::string FridgeStore::NowHmString() {
    const time_t now = std::time(nullptr);
    struct tm lt = {};
    localtime_r(&now, &lt);
    if (lt.tm_year + 1900 < 2025) return "--:--";
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%02d:%02d", lt.tm_hour, lt.tm_min);
    return buf;
}

int FridgeStore::ComputeItemAgeDays(const FridgeItem& it, int32_t today) {
    if (it.added_day <= 0) {
        // Relative age recorded before clock sync (0 = added today, -2 = 2 days ago)
        return static_cast<int>(std::clamp<int32_t>(-it.added_day, 0, 3650));
    }
    if (today <= 0) return 0;
    return static_cast<int>(std::clamp<int32_t>(today - it.added_day, 0, 3650));
}

int FridgeStore::ComputeItemDaysLeft(const FridgeItem& it, int32_t today, bool* has_expiry) {
    if (it.expire_day <= kUnsyncedExpiryThreshold) {
        if (has_expiry) *has_expiry = true;
        return static_cast<int>(kUnsyncedExpiryBase - it.expire_day);
    }
    if (it.expire_day > 0) {
        if (has_expiry) *has_expiry = true;
        if (today <= 0) return 0;
        return static_cast<int>(it.expire_day - today);
    }
    if (has_expiry) *has_expiry = false;
    return 999;
}

void FridgeStore::ReconcileUnsyncedDates() {
    const int32_t today = TodayDayNumber();
    if (today <= 0) return;
    std::lock_guard<std::mutex> lock(mutex_);
    bool changed = false;
    for (auto& it : items_) {
        if (it.added_day <= 0) {
            it.added_day = today + std::clamp<int32_t>(it.added_day, -365, 0);
            changed = true;
        }
        if (it.expire_day <= kUnsyncedExpiryThreshold) {
            const int32_t rel_days = std::clamp<int32_t>(kUnsyncedExpiryBase - it.expire_day, -365, 3650);
            it.expire_day = today + rel_days;
            changed = true;
        }
    }
    if (changed) {
        SaveToSpiffsLocked();
    }
}

bool FridgeStore::ConsumeStorageWarning(std::string* msg_out) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (storage_warning_.empty()) return false;
    if (msg_out) *msg_out = storage_warning_;
    storage_warning_.clear();
    return true;
}

esp_err_t FridgeStore::Init() {
    std::lock_guard<std::mutex> lock(mutex_);
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    LoadConfigFromNvsLocked();

    esp_vfs_spiffs_conf_t conf = {};
    conf.base_path = "/spiffs";
    conf.partition_label = "storage";
    conf.max_files = 6;
    conf.format_if_mount_failed = true;
    if (esp_vfs_spiffs_register(&conf) == ESP_OK) {
        spiffs_ready_ = true;
        LoadFromSpiffsLocked();
    } else {
        ESP_LOGE(kTag, "SPIFFS mount failed, using RAM starter data");
        SeedStarterDataLocked();
    }
    return ESP_OK;
}

void FridgeStore::LoadConfigFromNvsLocked() {
    nvs_handle_t h = 0;
    if (nvs_open(kNvsNs, NVS_READONLY, &h) != ESP_OK) return;
    config_.wifi_ssid = ReadNvsStr(h, "wifi_ssid", config_.wifi_ssid);
    config_.wifi_pass = ReadNvsStr(h, "wifi_pass", config_.wifi_pass);
    config_.ota_url = ReadNvsStr(h, "ota_url", config_.ota_url);
    config_.ws_token = ReadNvsStr(h, "ws_token", config_.ws_token);
    config_.nfc_shop_url = ReadNvsStr(h, "nfc_url", config_.nfc_shop_url);
    int32_t v = 0;
    if (nvs_get_i32(h, "sleep_min", &v) == ESP_OK) {
        config_.sleep_minutes = std::clamp<int>(v, 0, 720);
    }
    if (nvs_get_i32(h, "wake_hr", &v) == ESP_OK) {
        config_.wake_hour = std::clamp<int>(v, 0, 23);
    }
    if (nvs_get_i32(h, "ui_theme", &v) == ESP_OK) {
        config_.ui_theme = std::clamp<int>(v, 0, 1);
    }
    if (nvs_get_i32(h, "stby_sty", &v) == ESP_OK) {
        config_.standby_style = std::clamp<int>(v, 0, 4);
    }
    nvs_close(h);
}

AppConfig FridgeStore::GetConfig() {
    std::lock_guard<std::mutex> lock(mutex_);
    return config_;
}

esp_err_t FridgeStore::SaveConfig(const AppConfig& cfg) {
    std::lock_guard<std::mutex> lock(mutex_);
    config_ = cfg;
    config_.sleep_minutes = std::clamp(config_.sleep_minutes, 0, 720);
    config_.wake_hour = std::clamp(config_.wake_hour, 0, 23);
    config_.ui_theme = std::clamp(config_.ui_theme, 0, 1);
    config_.standby_style = std::clamp(config_.standby_style, 0, 4);
    nvs_handle_t h = 0;
    esp_err_t err = nvs_open(kNvsNs, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    nvs_set_str(h, "wifi_ssid", config_.wifi_ssid.c_str());
    nvs_set_str(h, "wifi_pass", config_.wifi_pass.c_str());
    nvs_set_str(h, "ota_url", config_.ota_url.c_str());
    nvs_set_str(h, "ws_token", config_.ws_token.c_str());
    nvs_set_str(h, "nfc_url", config_.nfc_shop_url.c_str());
    nvs_set_i32(h, "sleep_min", config_.sleep_minutes);
    nvs_set_i32(h, "wake_hr", config_.wake_hour);
    nvs_set_i32(h, "ui_theme", config_.ui_theme);
    nvs_set_i32(h, "stby_sty", config_.standby_style);
    err = nvs_commit(h);
    nvs_close(h);
    return err;
}

void FridgeStore::SeedDefaultRecipesLocked() {
    recipes_ = {
        {
            "番茄滑蛋",
            "4步快手家常菜，优先消耗圣女果和鸡蛋",
            12,
            {{"圣女果", "1盒"}, {"土鸡蛋", "3个"}, {"小香葱", "1把"}, {"盐", "少许"}},
            {
                {"鸡蛋打散加少许盐，圣女果对半切开，香葱切花。", 0, "备菜"},
                {"热锅凉油倒入蛋液，快速划散至八成熟盛出。", 2, "中小火"},
                {"底油下圣女果翻炒出汁，加少许白糖和盐。", 3, "中火"},
                {"倒回滑蛋翻炒裹匀茄汁，撒葱花出锅！", 1, "大火"},
            },
            false,
        },
        {
            "虾仁滑蛋",
            "高蛋白鲜嫩快手菜，消耗鲜虾仁和鸡蛋",
            10,
            {{"鲜虾仁", "200g"}, {"土鸡蛋", "3个"}, {"料酒", "1勺"}, {"盐", "少许"}},
            {
                {"虾仁加料酒胡椒腌制，鸡蛋加少许盐打散。", 5, "腌制"},
                {"热锅少油把虾仁炒至变红卷曲盛出。", 2, "中火"},
                {"虾仁拌入蛋液，小火慢推至嫩滑凝固出锅！", 2, "小火"},
            },
            false,
        },
        {
            "蒜蓉清炒菠菜",
            "清脆解腻，趁嫩把菠菜吃掉",
            8,
            {{"嫩菠菜", "1把"}, {"大蒜", "3瓣"}, {"盐", "少许"}},
            {
                {"菠菜洗净切段，水开焯烫去草酸后捞出。", 1, "焯水"},
                {"热锅爆香蒜末，倒入菠菜大火快炒加盐出锅！", 2, "大火"},
            },
            false,
        },
    };
}

void FridgeStore::SeedStarterDataLocked() {
    const int32_t today = TodayDayNumber();
    const auto mk_day = [&](int rel) -> int32_t {
        return (today > 0) ? (today + rel) : rel;
    };
    items_ = {
        {next_id_++, "鲜牛奶", "1盒", "冷藏", mk_day(-1), 0, ""},
        {next_id_++, "圣女果", "1盒", "冷藏", mk_day(-1), 0, ""},
        {next_id_++, "鲜虾仁", "200g", "冷藏", mk_day(0), 0, ""},
        {next_id_++, "土鸡蛋", "12个", "冷藏", mk_day(-2), 0, ""},
        {next_id_++, "嫩菠菜", "1把", "冷藏", mk_day(0), 0, ""},
        {next_id_++, "猪排骨", "500g", "冷冻", mk_day(-3), 0, ""},
    };
    shopping_ = {
        {next_id_++, "小香葱", "1把", "番茄滑蛋缺料", false},
        {next_id_++, "全麦面包", "1袋", "冰箱补货", false},
        {next_id_++, "生抽酱油", "1瓶", "冰箱补货", false},
    };
    SeedDefaultRecipesLocked();
}

bool FridgeStore::TryLoadJsonFileLocked(const char* path, bool* file_existed) {
    if (file_existed) *file_existed = FileExists(path);
    FILE* f = std::fopen(path, "rb");
    if (f == nullptr) return false;

    std::fseek(f, 0, SEEK_END);
    const long sz = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > 256 * 1024) {
        std::fclose(f);
        return false;
    }
    std::string buf(static_cast<size_t>(sz), '\0');
    const size_t rd = std::fread(buf.data(), 1, static_cast<size_t>(sz), f);
    std::fclose(f);
    if (rd != static_cast<size_t>(sz)) return false;

    cJSON* root = cJSON_Parse(buf.c_str());
    if (root == nullptr) return false;

    next_id_ = static_cast<uint32_t>(std::max(1, JInt(root, "next_id", 1)));
    items_.clear();
    if (cJSON* arr = cJSON_GetObjectItemCaseSensitive(root, "items"); cJSON_IsArray(arr)) {
        cJSON* el = nullptr;
        cJSON_ArrayForEach(el, arr) {
            FridgeItem it;
            it.id = static_cast<uint32_t>(JInt(el, "id", next_id_++));
            it.name = JStr(el, "name");
            it.qty = JStr(el, "qty", "1份");
            it.zone = JStr(el, "zone", "冷藏");
            it.added_day = JInt(el, "added_day", TodayDayNumber());
            it.expire_day = JInt(el, "expire_day", 0);
            it.note = JStr(el, "note");
            it.storage_reminded = JBool(el, "reminded", false);
            if (!it.name.empty()) {
                next_id_ = std::max(next_id_, it.id + 1);
                items_.push_back(it);
            }
        }
    }

    shopping_.clear();
    if (cJSON* arr = cJSON_GetObjectItemCaseSensitive(root, "shopping"); cJSON_IsArray(arr)) {
        cJSON* el = nullptr;
        cJSON_ArrayForEach(el, arr) {
            ShoppingItem sh;
            sh.id = static_cast<uint32_t>(JInt(el, "id", next_id_++));
            sh.name = JStr(el, "name");
            sh.qty = JStr(el, "qty", "1份");
            sh.source = JStr(el, "source");
            sh.checked = JBool(el, "checked", false);
            if (!sh.name.empty()) {
                next_id_ = std::max(next_id_, sh.id + 1);
                shopping_.push_back(sh);
            }
        }
    }

    recipes_.clear();
    if (cJSON* rarr = cJSON_GetObjectItemCaseSensitive(root, "recipes"); cJSON_IsArray(rarr)) {
        cJSON* r = nullptr;
        cJSON_ArrayForEach(r, rarr) {
            RecipeCard card;
            card.title = JStr(r, "title");
            card.summary = JStr(r, "summary");
            card.time_min = std::clamp(JInt(r, "time_min", 15), 1, 240);
            card.custom = JBool(r, "custom", false);
            if (cJSON* iarr = cJSON_GetObjectItemCaseSensitive(r, "ingredients"); cJSON_IsArray(iarr)) {
                cJSON* ig = nullptr;
                cJSON_ArrayForEach(ig, iarr) {
                    const std::string n = JStr(ig, "name");
                    if (!n.empty()) card.ingredients.push_back({n, JStr(ig, "amount", "适量")});
                }
            }
            if (cJSON* sarr = cJSON_GetObjectItemCaseSensitive(r, "steps"); cJSON_IsArray(sarr)) {
                cJSON* st = nullptr;
                cJSON_ArrayForEach(st, sarr) {
                    if (cJSON_IsObject(st)) {
                        const std::string t = JStr(st, "text");
                        if (!t.empty()) {
                            card.steps.push_back({t, std::clamp(JInt(st, "timer_min", 0), 0, 240), JStr(st, "heat")});
                        }
                    } else if (cJSON_IsString(st) && st->valuestring) {
                        card.steps.push_back({st->valuestring, 0, ""});
                    }
                }
            }
            if (!card.title.empty() && !card.steps.empty()) recipes_.push_back(card);
        }
    }
    cJSON_Delete(root);
    if (recipes_.empty()) {
        SeedDefaultRecipesLocked();
    }
    return true;
}

void FridgeStore::LoadFromSpiffsLocked() {
    bool main_existed = false;
    if (TryLoadJsonFileLocked(kDataPath, &main_existed)) {
        return;
    }

    bool bak_existed = false;
    if (TryLoadJsonFileLocked(kBakPath, &bak_existed)) {
        ESP_LOGW(kTag, "Recovered inventory from backup %s", kBakPath);
        SaveToSpiffsLocked();
        return;
    }

    if (main_existed || bak_existed) {
        // NEVER overwrite corrupt user archive with fake starter demo items!
        ESP_LOGE(kTag, "Existing SPIFFS archive unreadable; preserving at %s", kCorruptPath);
        if (main_existed) {
            std::remove(kCorruptPath);
            std::rename(kDataPath, kCorruptPath);
        }
        items_.clear();
        shopping_.clear();
        SeedDefaultRecipesLocked();
        storage_warning_ = "存档异常已保护备份，未覆盖原文件";
        return;
    }

    // Genuine first boot on fresh partition
    SeedStarterDataLocked();
    SaveToSpiffsLocked();
}

void FridgeStore::SaveToSpiffsLocked() {
    if (!spiffs_ready_) return;
    cJSON* root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "next_id", next_id_);

    cJSON* arr = cJSON_AddArrayToObject(root, "items");
    for (const auto& it : items_) {
        cJSON* o = cJSON_CreateObject();
        cJSON_AddNumberToObject(o, "id", it.id);
        cJSON_AddStringToObject(o, "name", it.name.c_str());
        cJSON_AddStringToObject(o, "qty", it.qty.c_str());
        cJSON_AddStringToObject(o, "zone", it.zone.c_str());
        cJSON_AddNumberToObject(o, "added_day", it.added_day);
        cJSON_AddNumberToObject(o, "expire_day", it.expire_day);
        cJSON_AddStringToObject(o, "note", it.note.c_str());
        if (it.storage_reminded) cJSON_AddBoolToObject(o, "reminded", true);
        cJSON_AddItemToArray(arr, o);
    }

    cJSON* sarr = cJSON_AddArrayToObject(root, "shopping");
    for (const auto& sh : shopping_) {
        cJSON* o = cJSON_CreateObject();
        cJSON_AddNumberToObject(o, "id", sh.id);
        cJSON_AddStringToObject(o, "name", sh.name.c_str());
        cJSON_AddStringToObject(o, "qty", sh.qty.c_str());
        cJSON_AddStringToObject(o, "source", sh.source.c_str());
        cJSON_AddBoolToObject(o, "checked", sh.checked);
        cJSON_AddItemToArray(sarr, o);
    }

    cJSON* rarr = cJSON_AddArrayToObject(root, "recipes");
    for (const auto& r : recipes_) {
        cJSON* o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "title", r.title.c_str());
        cJSON_AddStringToObject(o, "summary", r.summary.c_str());
        cJSON_AddNumberToObject(o, "time_min", r.time_min);
        cJSON_AddBoolToObject(o, "custom", r.custom);
        cJSON* ia = cJSON_AddArrayToObject(o, "ingredients");
        for (const auto& ig : r.ingredients) {
            cJSON* io = cJSON_CreateObject();
            cJSON_AddStringToObject(io, "name", ig.name.c_str());
            cJSON_AddStringToObject(io, "amount", ig.amount.c_str());
            cJSON_AddItemToArray(ia, io);
        }
        cJSON* sa = cJSON_AddArrayToObject(o, "steps");
        for (const auto& st : r.steps) {
            cJSON* so = cJSON_CreateObject();
            cJSON_AddStringToObject(so, "text", st.text.c_str());
            cJSON_AddNumberToObject(so, "timer_min", st.timer_min);
            cJSON_AddStringToObject(so, "heat", st.heat.c_str());
            cJSON_AddItemToArray(sa, so);
        }
        cJSON_AddItemToArray(rarr, o);
    }

    char* printed = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!printed) return;

    const size_t len = std::strlen(printed);
    std::remove(kTmpPath);
    FILE* f = std::fopen(kTmpPath, "wb");
    bool write_ok = false;
    if (f) {
        const size_t wr = std::fwrite(printed, 1, len, f);
        const int fl = std::fflush(f);
        fsync(fileno(f));
        const int cl = std::fclose(f);
        write_ok = (wr == len && fl == 0 && cl == 0);
    }
    cJSON_free(printed);

    if (!write_ok) {
        ESP_LOGE(kTag, "Failed writing %s; leaving previous archive intact", kTmpPath);
        std::remove(kTmpPath);
        return;
    }

    const bool had_main = FileExists(kDataPath);
    if (had_main) {
        std::remove(kBakPath);
        std::rename(kDataPath, kBakPath);
    }
    if (std::rename(kTmpPath, kDataPath) != 0) {
        ESP_LOGE(kTag, "Rename %s -> %s failed; restoring backup", kTmpPath, kDataPath);
        if (had_main) std::rename(kBakPath, kDataPath);
    }
}

int FridgeStore::FindBestItemIndexLocked(const std::string& query) const {
    if (query.empty() || items_.empty()) return -1;
    // Tier 1: Exact name match
    for (size_t i = 0; i < items_.size(); ++i) {
        if (items_[i].name == query) return static_cast<int>(i);
    }
    // Tier 2: Exact canonical synonym (e.g. 西红柿 == 番茄, 牛奶 == 鲜牛奶; NOT 酸牛奶!)
    for (size_t i = 0; i < items_.size(); ++i) {
        if (AreExactAliases(query, items_[i].name)) return static_cast<int>(i);
    }
    // Tier 3: Closest-length substring match without conflicting modifiers
    int best_idx = -1;
    int best_diff = 999999;
    for (size_t i = 0; i < items_.size(); ++i) {
        if (IngredientMatchesFood(query, items_[i].name)) {
            const int diff = std::abs(static_cast<int>(items_[i].name.size()) -
                                      static_cast<int>(query.size()));
            if (diff < best_diff) {
                best_diff = diff;
                best_idx = static_cast<int>(i);
            }
        }
    }
    return best_idx;
}

std::vector<FridgeItem> FridgeStore::GetItemsFiltered(ZoneFilter filter,
                                                      int* total_all,
                                                      int* total_expiring,
                                                      int* total_fridge,
                                                      int* total_freezer) {
    std::lock_guard<std::mutex> lock(mutex_);
    const int32_t today = TodayDayNumber();
    int c_all = 0, c_exp = 0, c_fr = 0, c_fz = 0;
    std::vector<FridgeItem> out;
    for (const auto& it : items_) {
        c_all++;
        bool has_exp = false;
        const int left = ComputeItemDaysLeft(it, today, &has_exp);
        const bool is_exp = (has_exp && left <= 2);
        if (is_exp) c_exp++;
        if (it.zone == "冷冻") c_fz++;
        else c_fr++;

        bool keep = (filter == ZoneFilter::kAll) ||
                    (filter == ZoneFilter::kExpiring && is_exp) ||
                    (filter == ZoneFilter::kFridge && it.zone != "冷冻") ||
                    (filter == ZoneFilter::kFreezer && it.zone == "冷冻");
        if (keep) out.push_back(it);
    }
    std::sort(out.begin(), out.end(), [today](const FridgeItem& a, const FridgeItem& b) {
        bool a_has = false, b_has = false;
        const int a_left = ComputeItemDaysLeft(a, today, &a_has);
        const int b_left = ComputeItemDaysLeft(b, today, &b_has);
        if (a_has != b_has) return a_has;
        if (a_has && a_left != b_left) return a_left < b_left;
        const int a_age = ComputeItemAgeDays(a, today);
        const int b_age = ComputeItemAgeDays(b, today);
        if (a_age != b_age) return a_age > b_age;
        return a.id < b.id;
    });
    if (total_all) *total_all = c_all;
    if (total_expiring) *total_expiring = c_exp;
    if (total_fridge) *total_fridge = c_fr;
    if (total_freezer) *total_freezer = c_fz;
    return out;
}

bool FridgeStore::CanAddOrUpdateItem(const std::string& name) {
    const std::string clean_name = ClampUtf8Bytes(name, kMaxNameBytes);
    if (clean_name.empty()) return false;
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& it : items_) {
        if (it.name == clean_name) return true;
    }
    return items_.size() < kMaxItems;
}

bool FridgeStore::HasItemMatchingName(const std::string& name) {
    const std::string clean_name = ClampUtf8Bytes(name, kMaxNameBytes);
    if (clean_name.empty()) return false;
    std::lock_guard<std::mutex> lock(mutex_);
    return FindBestItemIndexLocked(clean_name) >= 0;
}

uint32_t FridgeStore::AddOrUpdateItem(const std::string& name, const std::string& qty,
                                      const std::string& zone, int days_from_today,
                                      const std::string& note) {
    const std::string clean_name = ClampUtf8Bytes(name, kMaxNameBytes);
    if (clean_name.empty()) return 0;
    const std::string clean_qty = ClampUtf8Bytes(qty, kMaxQtyBytes);
    const std::string clean_zone = NormalizeZone(ClampUtf8Bytes(zone, kMaxZoneBytes));
    const std::string clean_note = ClampUtf8Bytes(note, kMaxNoteBytes);

    std::lock_guard<std::mutex> lock(mutex_);
    const int32_t today = TodayDayNumber();
    const FoodHint hint = food_catalog::Lookup(clean_name);
    const std::string eff_zone = clean_zone.empty() ? hint.zone : clean_zone;

    int32_t eff_expire = 0;
    if (days_from_today > -900) {
        const int32_t clamped_days = std::clamp<int32_t>(days_from_today, -365, 3650);
        eff_expire = (today > 0) ? (today + clamped_days) : (kUnsyncedExpiryBase - clamped_days);
    }

    for (auto& it : items_) {
        if (it.name == clean_name) {
            if (!clean_qty.empty()) {
                it.qty = ClampUtf8Bytes(MergeQuantityStrings(it.qty, clean_qty), kMaxQtyBytes);
            }
            if (!clean_zone.empty() && it.zone != eff_zone) {
                it.zone = eff_zone;
                it.storage_reminded = false;  // fridge <-> freezer: different storage time
            }
            // Preserve existing added_day so older batch age is not wiped on restock
            if (days_from_today > -900) {
                it.expire_day = eff_expire;
                it.storage_reminded = false;
            }
            if (!clean_note.empty()) it.note = clean_note;
            SaveToSpiffsLocked();
            return it.id;
        }
    }
    // Never silently delete the user's oldest item when full! Return 0 so caller alerts the user.
    if (items_.size() >= kMaxItems) {
        ESP_LOGW(kTag, "Inventory full (%u/%u), refusing to add '%s'",
                 static_cast<unsigned>(items_.size()), static_cast<unsigned>(kMaxItems),
                 clean_name.c_str());
        return 0;
    }
    FridgeItem it;
    it.id = next_id_++;
    it.name = clean_name;
    it.qty = clean_qty.empty() ? "1份" : clean_qty;
    it.zone = eff_zone;
    it.added_day = today;  // 0 if unsynced -> reconciled once RTC/SNTP syncs
    it.expire_day = eff_expire;
    it.note = clean_note;
    items_.push_back(it);
    SaveToSpiffsLocked();
    return it.id;
}

std::string FridgeStore::SetItemExpiryByName(const std::string& name, int days_from_today) {
    if (name.empty()) return "";
    std::lock_guard<std::mutex> lock(mutex_);
    const int idx = FindBestItemIndexLocked(name);
    if (idx < 0) return "";
    const int32_t today = TodayDayNumber();
    auto& it = items_[static_cast<size_t>(idx)];
    if (days_from_today <= -900) {
        it.expire_day = 0;
    } else {
        const int32_t clamped_days = std::clamp<int32_t>(days_from_today, -365, 3650);
        it.expire_day = (today > 0) ? (today + clamped_days) : (kUnsyncedExpiryBase - clamped_days);
    }
    it.storage_reminded = false;
    SaveToSpiffsLocked();
    return it.name;
}

bool FridgeStore::RemoveItemById(uint32_t id) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto n = items_.size();
    items_.erase(std::remove_if(items_.begin(), items_.end(),
                                [id](const FridgeItem& x) { return x.id == id; }),
                 items_.end());
    if (items_.size() != n) { SaveToSpiffsLocked(); return true; }
    return false;
}

bool FridgeStore::RemoveItemByName(const std::string& name) {
    if (name.empty()) return false;
    std::lock_guard<std::mutex> lock(mutex_);
    const int idx = FindBestItemIndexLocked(name);
    if (idx < 0) return false;
    items_.erase(items_.begin() + idx);
    SaveToSpiffsLocked();
    return true;
}

bool FridgeStore::AdjustItemExpiry(uint32_t id, int delta_days) {
    std::lock_guard<std::mutex> lock(mutex_);
    const int32_t clamped_delta = std::clamp<int32_t>(delta_days, -365, 365);
    const int32_t today = TodayDayNumber();
    for (auto& it : items_) {
        if (it.id == id) {
            bool has_exp = false;
            const int cur_left = ComputeItemDaysLeft(it, today, &has_exp);
            const int32_t new_left = std::clamp<int32_t>(
                has_exp ? (cur_left + clamped_delta) : std::max<int32_t>(1, clamped_delta),
                -365, 3650);
            it.expire_day = (today > 0) ? (today + new_left) : (kUnsyncedExpiryBase - new_left);
            it.storage_reminded = false;
            SaveToSpiffsLocked();
            return true;
        }
    }
    return false;
}

bool FridgeStore::DeductItemAmount(uint32_t id, const std::string& used_amount) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto it = items_.begin(); it != items_.end(); ++it) {
        if (it->id != id) continue;
        const ParsedQty cur = ParseQty(it->qty);
        const ParsedQty used = ParseQty(used_amount);

        if (cur.kind == QtyKind::kMass) {
            double deduct_g = 0.0;
            if (used.kind == QtyKind::kMass && used.value > 0.0) {
                deduct_g = used.value;
            } else {
                // Single-pack meat/seafood/veggie (<=550g) is used up in 1 dish; larger bulk subtracts 250g
                deduct_g = (cur.value <= 550.0) ? cur.value : 250.0;
            }
            const double rem_g = cur.value - deduct_g;
            if (rem_g <= 25.0) {
                items_.erase(it);
            } else {
                it->qty = FormatMassGrams(rem_g, cur.raw_unit == "克" ? "克" : "g");
            }
        } else if (cur.kind == QtyKind::kVolume) {
            const double deduct_ml = (used.kind == QtyKind::kVolume && used.value > 0.0)
                                         ? used.value
                                         : ((cur.value <= 350.0) ? cur.value : 250.0);
            const double rem_ml = cur.value - deduct_ml;
            if (rem_ml <= 25.0) {
                items_.erase(it);
            } else {
                it->qty = std::to_string(static_cast<int>(std::llround(rem_ml))) +
                          (cur.raw_unit == "毫升" ? "毫升" : "ml");
            }
        } else if (cur.kind == QtyKind::kCount) {
            int deduct_n = 1;
            if (used.kind == QtyKind::kCount && used.value >= 1.0 &&
                (used.raw_unit == cur.raw_unit || used.raw_unit == "个" || used.raw_unit == "份")) {
                deduct_n = static_cast<int>(std::llround(used.value));
            }
            const int rem_n = static_cast<int>(std::llround(cur.value)) - deduct_n;
            if (rem_n <= 0) {
                items_.erase(it);
            } else {
                it->qty = std::to_string(rem_n) + cur.raw_unit;
            }
        } else {
            items_.erase(it);
        }
        SaveToSpiffsLocked();
        return true;
    }
    return false;
}

bool FridgeStore::DecrementItemQty(uint32_t id) {
    return DeductItemAmount(id, "");
}

bool FridgeStore::MoveItemToShopping(uint32_t id) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto it = items_.begin(); it != items_.end(); ++it) {
        if (it->id == id) {
            bool merged_shop = false;
            for (auto& sh : shopping_) {
                if (sh.name == it->name) {
                    sh.qty = ClampUtf8Bytes(MergeQuantityStrings(sh.qty, it->qty), kMaxQtyBytes);
                    sh.source = "冰箱补货";
                    sh.checked = false;
                    merged_shop = true;
                    break;
                }
            }
            if (!merged_shop) {
                if (shopping_.size() >= kMaxShopping) {
                    ESP_LOGW(kTag, "Shopping list full (%u/%u), cannot move '%s'",
                             static_cast<unsigned>(shopping_.size()),
                             static_cast<unsigned>(kMaxShopping), it->name.c_str());
                    return false;
                }
                shopping_.push_back({next_id_++, it->name, it->qty, "冰箱补货", false});
            }
            items_.erase(it);
            SaveToSpiffsLocked();
            return true;
        }
    }
    return false;
}

RecipeMatchView FridgeStore::MatchSingleLocked(const RecipeCard& r, int32_t today) const {
    RecipeMatchView v;
    v.recipe = r;
    int non_pantry_total = 0;
    for (const auto& ig : r.ingredients) {
        MatchedIngredient mi;
        mi.name = ig.name;
        mi.amount = ig.amount;
        if (food_catalog::IsPantryStaple(ig.name)) {
            mi.state = MatchState::kPantry;
            v.matched.push_back(mi);
            continue;
        }
        non_pantry_total++;
        const FridgeItem* best = nullptr;
        int best_left = 999;
        bool best_has = false;
        for (const auto& fi : items_) {
            if (IngredientMatchesFood(ig.name, fi.name)) {
                bool fi_has = false;
                const int fi_left = ComputeItemDaysLeft(fi, today, &fi_has);
                if (best == nullptr || (fi_has && (!best_has || fi_left < best_left))) {
                    best = &fi;
                    best_has = fi_has;
                    best_left = fi_left;
                }
            }
        }
        if (best != nullptr) {
            mi.fridge_item_id = best->id;
            mi.fridge_qty = best->qty;
            mi.days_left = best_has ? best_left : 999;
            if (best_has && mi.days_left <= 3) {
                mi.state = MatchState::kHaveUrgent;
                v.urgent_hits++;
            } else {
                mi.state = MatchState::kHaveFresh;
            }
            v.have_count++;
        } else {
            mi.state = MatchState::kMissing;
            v.missing_count++;
        }
        v.matched.push_back(mi);
    }
    v.match_pct = (non_pantry_total > 0) ? (v.have_count * 100 / non_pantry_total) : 100;
    return v;
}

bool FridgeStore::HasRecipeForIngredient(const std::string& ingredient_name) {
    if (ingredient_name.empty()) return false;
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& r : recipes_) {
        if (r.title.find(ingredient_name) != std::string::npos) return true;
        for (const auto& ig : r.ingredients) {
            if (IngredientMatchesFood(ingredient_name, ig.name)) return true;
        }
    }
    return false;
}

std::vector<RecipeMatchView> FridgeStore::GetMatchedRecipesSorted(const std::string& focus_ingredient) {
    std::lock_guard<std::mutex> lock(mutex_);
    const int32_t today = TodayDayNumber();
    std::vector<RecipeMatchView> out;
    for (const auto& r : recipes_) {
        out.push_back(MatchSingleLocked(r, today));
    }
    auto has_focus = [&](const RecipeMatchView& mv) {
        if (focus_ingredient.empty()) return false;
        if (mv.recipe.title.find(focus_ingredient) != std::string::npos) return true;
        for (const auto& ig : mv.recipe.ingredients) {
            if (IngredientMatchesFood(focus_ingredient, ig.name)) return true;
        }
        return false;
    };
    std::stable_sort(out.begin(), out.end(), [&](const RecipeMatchView& a, const RecipeMatchView& b) {
        if (!focus_ingredient.empty()) {
            const bool fa = has_focus(a), fb = has_focus(b);
            if (fa != fb) return fa;
        }
        if (a.recipe.custom != b.recipe.custom) return a.recipe.custom;
        if (a.recipe.custom && b.recipe.custom) return false;
        if (a.urgent_hits != b.urgent_hits) return a.urgent_hits > b.urgent_hits;
        return a.match_pct > b.match_pct;
    });
    return out;
}

int FridgeStore::AddMissingToShopping(const RecipeMatchView& view) {
    int added = 0;
    for (const auto& m : view.matched) {
        if (m.state == MatchState::kMissing) {
            if (AddShoppingItem(m.name, m.amount.empty() ? "1份" : m.amount,
                                view.recipe.title + "缺料") != 0) {
                added++;
            }
        }
    }
    return added;
}

int FridgeStore::DeductCookedIngredients(const RecipeMatchView& view) {
    int deducted = 0;
    for (const auto& m : view.matched) {
        if ((m.state == MatchState::kHaveUrgent || m.state == MatchState::kHaveFresh) &&
            m.fridge_item_id != 0) {
            if (DeductItemAmount(m.fridge_item_id, m.amount)) deducted++;
        }
    }
    return deducted;
}

void FridgeStore::PushRecipe(const RecipeCard& recipe) {
    const std::string clean_title = ClampUtf8Bytes(recipe.title, kMaxNameBytes);
    if (clean_title.empty() || recipe.steps.empty()) return;
    std::lock_guard<std::mutex> lock(mutex_);
    RecipeCard pinned;
    pinned.title = clean_title;
    pinned.summary = ClampUtf8Bytes(recipe.summary, kMaxNoteBytes);
    pinned.custom = true;
    pinned.time_min = std::clamp(recipe.time_min, 1, 240);
    for (const auto& ig : recipe.ingredients) {
        if (pinned.ingredients.size() >= kMaxRecipeIngredients) break;
        const std::string iname = ClampUtf8Bytes(ig.name, kMaxNameBytes);
        if (!iname.empty()) {
            pinned.ingredients.push_back({iname, ClampUtf8Bytes(ig.amount, kMaxQtyBytes)});
        }
    }
    for (const auto& st : recipe.steps) {
        if (pinned.steps.size() >= kMaxRecipeSteps) break;
        const std::string stext = ClampUtf8Bytes(st.text, kMaxStepTextBytes);
        if (!stext.empty()) {
            pinned.steps.push_back({stext, std::clamp(st.timer_min, 0, 240),
                                    ClampUtf8Bytes(st.heat, kMaxQtyBytes)});
        }
    }
    if (pinned.steps.empty()) return;
    recipes_.erase(std::remove_if(recipes_.begin(), recipes_.end(),
                                  [&](const RecipeCard& r) { return r.title == pinned.title; }),
                   recipes_.end());
    recipes_.insert(recipes_.begin(), std::move(pinned));
    if (recipes_.size() > kMaxRecipes) recipes_.resize(kMaxRecipes);
    SaveToSpiffsLocked();
}

bool FridgeStore::RemoveRecipe(const std::string& title) {
    if (title.empty()) return false;
    std::lock_guard<std::mutex> lock(mutex_);
    const auto old_size = recipes_.size();
    recipes_.erase(std::remove_if(recipes_.begin(), recipes_.end(),
                                  [&](const RecipeCard& r) { return r.title == title; }),
                   recipes_.end());
    if (recipes_.size() == old_size) return false;
    if (recipes_.empty()) {
        SeedDefaultRecipesLocked();
    }
    SaveToSpiffsLocked();
    return true;
}

std::vector<ShoppingItem> FridgeStore::GetShoppingList() {
    std::lock_guard<std::mutex> lock(mutex_);
    return shopping_;
}

bool FridgeStore::CanAddShoppingItem(const std::string& name) {
    const std::string clean_name = ClampUtf8Bytes(name, kMaxNameBytes);
    if (clean_name.empty()) return false;
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& sh : shopping_) {
        if (sh.name == clean_name) return true;
    }
    return shopping_.size() < kMaxShopping;
}

uint32_t FridgeStore::AddShoppingItem(const std::string& name, const std::string& qty,
                                      const std::string& source) {
    const std::string clean_name = ClampUtf8Bytes(name, kMaxNameBytes);
    if (clean_name.empty()) return 0;
    const std::string clean_qty = ClampUtf8Bytes(qty, kMaxQtyBytes);
    const std::string clean_src = ClampUtf8Bytes(source, kMaxNoteBytes);

    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& sh : shopping_) {
        if (sh.name == clean_name) {
            if (!clean_qty.empty()) {
                sh.qty = ClampUtf8Bytes(MergeQuantityStrings(sh.qty, clean_qty), kMaxQtyBytes);
            }
            if (!clean_src.empty()) sh.source = clean_src;
            sh.checked = false;
            SaveToSpiffsLocked();
            return sh.id;
        }
    }
    if (shopping_.size() >= kMaxShopping) {
        ESP_LOGW(kTag, "Shopping list full (%u/%u), refusing '%s'",
                 static_cast<unsigned>(shopping_.size()),
                 static_cast<unsigned>(kMaxShopping), clean_name.c_str());
        return 0;
    }
    ShoppingItem sh;
    sh.id = next_id_++;
    sh.name = clean_name;
    sh.qty = clean_qty.empty() ? "1份" : clean_qty;
    sh.source = clean_src.empty() ? "手动添加" : clean_src;
    shopping_.push_back(sh);
    SaveToSpiffsLocked();
    return sh.id;
}

bool FridgeStore::ToggleShoppingItem(uint32_t id) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& sh : shopping_) {
        if (sh.id == id) { sh.checked = !sh.checked; SaveToSpiffsLocked(); return true; }
    }
    return false;
}

bool FridgeStore::RemoveShoppingItem(uint32_t id) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto n = shopping_.size();
    shopping_.erase(std::remove_if(shopping_.begin(), shopping_.end(),
                                   [id](const ShoppingItem& x) { return x.id == id; }),
                    shopping_.end());
    if (shopping_.size() != n) { SaveToSpiffsLocked(); return true; }
    return false;
}

int FridgeStore::CommitCheckedShoppingToFridge(int* skipped_full_out) {
    std::lock_guard<std::mutex> lock(mutex_);
    const int32_t today = TodayDayNumber();
    int moved = 0;
    int skipped_full = 0;
    std::vector<ShoppingItem> remain;
    for (const auto& sh : shopping_) {
        if (sh.checked) {
            const FoodHint h = food_catalog::Lookup(sh.name);
            bool merged = false;
            for (auto& it : items_) {
                if (it.name == sh.name) {
                    if (!sh.qty.empty()) {
                        it.qty = ClampUtf8Bytes(MergeQuantityStrings(it.qty, sh.qty), kMaxQtyBytes);
                    }
                    merged = true;
                    break;
                }
            }
            if (!merged) {
                if (items_.size() >= kMaxItems) {
                    // Never silently evict existing fridge items; keep item checked in shopping list!
                    skipped_full++;
                    remain.push_back(sh);
                    continue;
                }
                items_.push_back({next_id_++, sh.name, sh.qty.empty() ? "1份" : sh.qty,
                                  h.zone, today, 0, ""});
            }
            moved++;
        } else {
            remain.push_back(sh);
        }
    }
    if (skipped_full_out) *skipped_full_out = skipped_full;
    if (moved > 0) {
        shopping_.swap(remain);
        SaveToSpiffsLocked();
    }
    return moved;
}

int FridgeStore::CommitAllShoppingToFridge(const std::vector<std::string>& filter_names,
                                           std::vector<std::string>* moved_names_out,
                                           int* skipped_full_out) {
    std::lock_guard<std::mutex> lock(mutex_);
    const int32_t today = TodayDayNumber();
    int moved = 0;
    int skipped_full = 0;
    std::vector<ShoppingItem> remain;

    auto matches_filter = [&](const std::string& item_name) -> bool {
        if (filter_names.empty()) return true;
        for (const auto& q : filter_names) {
            if (q.empty()) continue;
            if (item_name == q || AreExactAliases(q, item_name) ||
                item_name.find(q) != std::string::npos ||
                q.find(item_name) != std::string::npos) {
                return true;
            }
        }
        return false;
    };

    for (const auto& sh : shopping_) {
        if (matches_filter(sh.name)) {
            const FoodHint h = food_catalog::Lookup(sh.name);
            bool merged = false;
            for (auto& it : items_) {
                if (it.name == sh.name || AreExactAliases(sh.name, it.name)) {
                    if (!sh.qty.empty()) {
                        it.qty = ClampUtf8Bytes(MergeQuantityStrings(it.qty, sh.qty), kMaxQtyBytes);
                    }
                    it.added_day = today;
                    it.storage_reminded = false;
                    merged = true;
                    break;
                }
            }
            if (!merged) {
                if (items_.size() >= kMaxItems) {
                    skipped_full++;
                    remain.push_back(sh);
                    continue;
                }
                items_.push_back({next_id_++, sh.name, sh.qty.empty() ? "1份" : sh.qty,
                                  h.zone, today, 0, ""});
            }
            if (moved_names_out) {
                moved_names_out->push_back(
                    sh.name + ((sh.qty.empty() || sh.qty == "1份") ? "" : (" " + sh.qty)));
            }
            moved++;
        } else {
            remain.push_back(sh);
        }
    }
    if (skipped_full_out) *skipped_full_out = skipped_full;
    if (moved > 0) {
        shopping_.swap(remain);
        SaveToSpiffsLocked();
    }
    return moved;
}

std::vector<KitchenTimer> FridgeStore::GetTimers() {
    std::lock_guard<std::mutex> lock(mutex_);
    return timers_;
}

bool FridgeStore::StartTimer(const std::string& label, int minutes, int seconds) {
    const int64_t m = std::clamp<int64_t>(minutes, 0, 1440);
    const int64_t s = std::clamp<int64_t>(seconds, 0, 86400);
    const int64_t total64 = m * 60 + s;
    if (total64 <= 0 || total64 > 86400) return false;
    const int total = static_cast<int>(total64);

    std::lock_guard<std::mutex> lock(mutex_);
    const int64_t now_ms = esp_timer_get_time() / 1000;
    const std::string eff_label = label.empty() ? "厨房计时" : ClampUtf8Bytes(label, kMaxNameBytes);
    const int64_t end_ms = now_ms + total64 * 1000;

    timers_.erase(std::remove_if(timers_.begin(), timers_.end(),
                                 [](const KitchenTimer& t) { return !t.active && !t.ringing; }),
                  timers_.end());

    bool replaced = false;
    for (auto& t : timers_) {
        if (t.active && !t.ringing && t.label == eff_label) {
            t.total_sec = total;
            t.end_uptime_ms = end_ms;
            replaced = true;
            break;
        }
    }
    if (!replaced) {
        if (timers_.size() >= kMaxTimers) timers_.erase(timers_.begin());
        timers_.push_back({next_id_++, eff_label, total, end_ms, true, false});
    }

    std::stable_sort(timers_.begin(), timers_.end(), [](const KitchenTimer& a, const KitchenTimer& b) {
        if (a.ringing != b.ringing) return a.ringing;
        return a.end_uptime_ms < b.end_uptime_ms;
    });
    return true;
}

bool FridgeStore::StopTimer(uint32_t id) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto n = timers_.size();
    timers_.erase(std::remove_if(timers_.begin(), timers_.end(),
                                 [id](const KitchenTimer& t) { return t.id == id; }),
                  timers_.end());
    return timers_.size() != n;
}

bool FridgeStore::StopAllRinging() {
    std::lock_guard<std::mutex> lock(mutex_);
    bool any = false;
    timers_.erase(std::remove_if(timers_.begin(), timers_.end(),
                                 [&any](const KitchenTimer& t) {
                                     if (t.ringing) { any = true; return true; }
                                     return false;
                                 }),
                  timers_.end());
    return any;
}

bool FridgeStore::StopAllTimers() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (timers_.empty()) return false;
    timers_.clear();
    return true;
}

bool FridgeStore::TickTimers(int64_t now_uptime_ms, bool* any_ringing, bool* any_active) {
    std::lock_guard<std::mutex> lock(mutex_);
    bool newly = false;
    *any_ringing = false;
    *any_active = false;
    for (auto& t : timers_) {
        if (t.active && now_uptime_ms >= t.end_uptime_ms) {
            t.active = false;
            t.ringing = true;
            newly = true;
        }
        if (t.ringing) *any_ringing = true;
        if (t.active) *any_active = true;
    }
    return newly;
}

void FridgeStore::SaveUndoSnapshot() {
    std::lock_guard<std::mutex> lock(mutex_);
    undo_items_ = items_;
    undo_shopping_ = shopping_;
    undo_recipes_ = recipes_;
    has_undo_ = true;
}

bool FridgeStore::HasUndoSnapshot() {
    std::lock_guard<std::mutex> lock(mutex_);
    return has_undo_;
}

bool FridgeStore::UndoLastVoiceAction() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!has_undo_) return false;
    items_.swap(undo_items_);
    shopping_.swap(undo_shopping_);
    recipes_.swap(undo_recipes_);
    has_undo_ = false;
    SaveToSpiffsLocked();
    return true;
}

LocalFastAction FridgeStore::TryLocalFastCommand(const std::string& text) {
    LocalFastAction res;
    if (text.empty() || text.size() > 36) return res;

    // Compound commands with inventory/recipe intent always go to LLM!
    const bool looks_compound =
        text.find("放") != std::string::npos || text.find("买") != std::string::npos ||
        text.find("吃") != std::string::npos || text.find("过期") != std::string::npos ||
        text.find("菜谱") != std::string::npos || text.find("推荐") != std::string::npos;
    if (looks_compound) return res;

    if (text.find("下一步") != std::string::npos || text.find("下个步骤") != std::string::npos) {
        res.handled = true; res.step_delta = 1; res.switch_to_cook = true;
        res.receipt = "已翻到下一步";
        return res;
    }
    if (text.find("上一步") != std::string::npos || text.find("上个步骤") != std::string::npos) {
        res.handled = true; res.step_delta = -1; res.switch_to_cook = true;
        res.receipt = "已回到上一步";
        return res;
    }
    if (text.find("取消计时") != std::string::npos || text.find("停止计时") != std::string::npos ||
        text.find("关闭闹铃") != std::string::npos) {
        StopAllTimers();
        res.handled = true; res.stopped_timers = true;
        res.receipt = "已停止全部计时";
        return res;
    }
    if (text.find("定时") != std::string::npos || text.find("倒计时") != std::string::npos ||
        text.find("提醒我") != std::string::npos) {
        const int total_sec = ParseSpokenDurationSeconds(text);
        if (total_sec > 0 && StartTimer("语音计时", total_sec / 60, total_sec % 60)) {
            res.handled = true;
            res.switch_to_cook = true;
            if (total_sec >= 60 && total_sec % 60 == 0) {
                res.receipt = "已定 " + std::to_string(total_sec / 60) + "分钟计时";
            } else if (total_sec >= 60) {
                res.receipt = "已定 " + std::to_string(total_sec / 60) + "分" +
                              std::to_string(total_sec % 60) + "秒计时";
            } else {
                res.receipt = "已定 " + std::to_string(total_sec) + "秒计时";
            }
            return res;
        }
    }
    return res;
}

void FridgeStore::RecordAiLog(const std::string& input, const std::string& output,
                              int latency_ms, bool ok) {
    std::lock_guard<std::mutex> lock(mutex_);
    ai_logs_.insert(ai_logs_.begin(), {NowHmString(), input, output, latency_ms, ok});
    if (ai_logs_.size() > kMaxAiLogs) ai_logs_.resize(kMaxAiLogs);
}

std::string FridgeStore::BuildPromptContext(const std::string& focus_ingredient) {
    std::lock_guard<std::mutex> lock(mutex_);
    const int32_t today = TodayDayNumber();
    std::string ctx = "今天:" + FormatDate(today) + "\n冰箱现有库存:\n";
    if (items_.empty()) {
        ctx += "(空)\n";
    } else {
        for (const auto& it : items_) {
            ctx += "- " + it.name + " " + it.qty + " [" + it.zone + "] ";
            bool has_exp = false;
            const int left = ComputeItemDaysLeft(it, today, &has_exp);
            if (has_exp) {
                ctx += (left < 0 ? ("过期" + std::to_string(-left) + "天\n")
                                 : (left == 0 ? "今天到期\n" : ("剩" + std::to_string(left) + "天\n")));
            } else {
                const int age = ComputeItemAgeDays(it, today);
                ctx += (age == 0 ? "今天放入\n" : ("已放" + std::to_string(age) + "天\n"));
            }
        }
    }
    if (!focus_ingredient.empty()) {
        ctx += "用户想用这样食材做菜: " + focus_ingredient + "（请推荐以它为主料的经典家常菜，不要硬拼其他库存）\n";
    }
    ctx += "采购清单:\n";
    if (shopping_.empty()) {
        ctx += "(空)\n";
    } else {
        for (const auto& sh : shopping_) {
            ctx += std::string(sh.checked ? "- [已买]" : "- [待买]") + sh.name + " " + sh.qty + "\n";
        }
    }
    return ctx;
}

std::string FridgeStore::ExportStateJson(const std::string& ip, bool wifi_ok, bool ap_mode,
                                         int battery_pct, bool charging) {
    std::lock_guard<std::mutex> lock(mutex_);
    const int32_t today = TodayDayNumber();
    const int64_t now_ms = esp_timer_get_time() / 1000;

    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "today_str", FormatDate(today).c_str());
    cJSON_AddStringToObject(root, "ip", ip.c_str());
    cJSON_AddBoolToObject(root, "wifi_ok", wifi_ok);
    cJSON_AddBoolToObject(root, "ap_mode", ap_mode);
    cJSON_AddNumberToObject(root, "battery", battery_pct);
    cJSON_AddBoolToObject(root, "charging", charging);

    auto sorted = items_;
    std::sort(sorted.begin(), sorted.end(), [today](const FridgeItem& a, const FridgeItem& b) {
        bool a_has = false, b_has = false;
        const int a_left = ComputeItemDaysLeft(a, today, &a_has);
        const int b_left = ComputeItemDaysLeft(b, today, &b_has);
        if (a_has != b_has) return a_has;
        if (a_has && a_left != b_left) return a_left < b_left;
        const int a_age = ComputeItemAgeDays(a, today);
        const int b_age = ComputeItemAgeDays(b, today);
        if (a_age != b_age) return a_age > b_age;
        return a.id < b.id;
    });
    cJSON* arr = cJSON_AddArrayToObject(root, "items");
    for (const auto& it : sorted) {
        bool has_exp = false;
        const int left = ComputeItemDaysLeft(it, today, &has_exp);
        const int age = ComputeItemAgeDays(it, today);
        cJSON* o = cJSON_CreateObject();
        cJSON_AddNumberToObject(o, "id", it.id);
        cJSON_AddStringToObject(o, "name", it.name.c_str());
        cJSON_AddStringToObject(o, "qty", it.qty.c_str());
        cJSON_AddStringToObject(o, "zone", it.zone.c_str());
        cJSON_AddNumberToObject(o, "days_left", has_exp ? left : 999);
        cJSON_AddNumberToObject(o, "age_days", age);
        cJSON_AddStringToObject(o, "expire_str",
                                has_exp ? (it.expire_day > 0 ? FormatDate(it.expire_day).c_str() : "待校时")
                                        : "未设到期");
        cJSON_AddStringToObject(o, "note", it.note.c_str());
        cJSON_AddItemToArray(arr, o);
    }

    cJSON* sh_arr = cJSON_AddArrayToObject(root, "shopping");
    for (const auto& sh : shopping_) {
        cJSON* o = cJSON_CreateObject();
        cJSON_AddNumberToObject(o, "id", sh.id);
        cJSON_AddStringToObject(o, "name", sh.name.c_str());
        cJSON_AddStringToObject(o, "qty", sh.qty.c_str());
        cJSON_AddStringToObject(o, "source", sh.source.c_str());
        cJSON_AddBoolToObject(o, "checked", sh.checked);
        cJSON_AddItemToArray(sh_arr, o);
    }

    cJSON* t_arr = cJSON_AddArrayToObject(root, "timers");
    for (const auto& t : timers_) {
        cJSON* o = cJSON_CreateObject();
        cJSON_AddNumberToObject(o, "id", t.id);
        cJSON_AddStringToObject(o, "label", t.label.c_str());
        const int64_t rem_ms = t.active ? std::max<int64_t>(0, t.end_uptime_ms - now_ms) : 0;
        const int rem = t.active ? static_cast<int>((rem_ms + 999) / 1000) : 0;
        cJSON_AddNumberToObject(o, "remaining_sec", rem);
        cJSON_AddBoolToObject(o, "ringing", t.ringing);
        cJSON_AddItemToArray(t_arr, o);
    }

    std::vector<RecipeMatchView> rvs;
    rvs.reserve(recipes_.size());
    for (const auto& r : recipes_) rvs.push_back(MatchSingleLocked(r, today));
    std::stable_sort(rvs.begin(), rvs.end(), [](const RecipeMatchView& a, const RecipeMatchView& b) {
        if (a.recipe.custom != b.recipe.custom) return a.recipe.custom;
        if (a.recipe.custom && b.recipe.custom) return false;
        if (a.urgent_hits != b.urgent_hits) return a.urgent_hits > b.urgent_hits;
        if (a.match_pct != b.match_pct) return a.match_pct > b.match_pct;
        return a.missing_count < b.missing_count;
    });

    cJSON* r_arr = cJSON_AddArrayToObject(root, "recipes");
    for (const auto& mv : rvs) {
        const auto& r = mv.recipe;
        cJSON* o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "title", r.title.c_str());
        cJSON_AddStringToObject(o, "summary", r.summary.c_str());
        cJSON_AddNumberToObject(o, "time_min", r.time_min);
        cJSON_AddBoolToObject(o, "custom", r.custom);
        cJSON_AddNumberToObject(o, "match_pct", mv.match_pct);
        cJSON* sa = cJSON_AddArrayToObject(o, "steps");
        for (const auto& st : r.steps) {
            cJSON_AddItemToArray(sa, cJSON_CreateString(st.text.c_str()));
        }
        cJSON_AddItemToArray(r_arr, o);
    }

    cJSON* l_arr = cJSON_AddArrayToObject(root, "ai_logs");
    for (const auto& lg : ai_logs_) {
        cJSON* o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "time", lg.time_str.c_str());
        cJSON_AddStringToObject(o, "input", lg.input.c_str());
        cJSON_AddStringToObject(o, "output", lg.output.c_str());
        cJSON_AddNumberToObject(o, "ms", lg.latency_ms);
        cJSON_AddBoolToObject(o, "ok", lg.ok);
        cJSON_AddItemToArray(l_arr, o);
    }

    cJSON* cfg = cJSON_AddObjectToObject(root, "config");
    cJSON_AddStringToObject(cfg, "wifi_ssid", config_.wifi_ssid.c_str());
    cJSON_AddStringToObject(cfg, "ota_url", config_.ota_url.c_str());
    cJSON_AddStringToObject(cfg, "nfc_shop_url", config_.nfc_shop_url.c_str());
    cJSON_AddBoolToObject(cfg, "has_ws_token", !config_.ws_token.empty());
    cJSON_AddNumberToObject(cfg, "sleep_minutes", config_.sleep_minutes);
    cJSON_AddNumberToObject(cfg, "wake_hour", config_.wake_hour);
    cJSON_AddNumberToObject(cfg, "ui_theme", config_.ui_theme);
    cJSON_AddNumberToObject(cfg, "standby_style", config_.standby_style);

    char* printed = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!printed) return "{}";
    std::string out(printed);
    cJSON_free(printed);
    return out;
}

int32_t FridgeStore::GetLastReminderDay() {
    std::lock_guard<std::mutex> lock(mutex_);
    nvs_handle_t h = 0;
    int32_t v = 0;
    if (nvs_open(kNvsNs, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_i32(h, "rem_day", &v);
        nvs_close(h);
    }
    return v;
}

void FridgeStore::SetLastReminderDay(int32_t day) {
    std::lock_guard<std::mutex> lock(mutex_);
    nvs_handle_t h = 0;
    if (nvs_open(kNvsNs, NVS_READWRITE, &h) != ESP_OK) return;
    if (nvs_set_i32(h, "rem_day", day) == ESP_OK) nvs_commit(h);
    nvs_close(h);
}

std::string FridgeStore::BuildStorageReminderText(std::vector<uint32_t>* ids_out) {
    if (ids_out) ids_out->clear();
    std::lock_guard<std::mutex> lock(mutex_);
    const int32_t today = TodayDayNumber();
    struct Nudge { uint32_t id; std::string name; int age; double ratio; };
    std::vector<Nudge> list;
    for (const auto& it : items_) {
        if (it.storage_reminded) continue;
        const int age = ComputeItemAgeDays(it, today);
        if (age <= 0) continue;
        int limit = 0;
        bool has_exp = false;
        const int left = ComputeItemDaysLeft(it, today, &has_exp);
        if (has_exp) {
            limit = age + left;  // the user's own storage days win
        } else if (it.zone == "冷冻") {
            limit = food_catalog::LookupStorageGuide(it.name).freezer_days;
        } else if (it.zone == "冷藏") {
            limit = food_catalog::LookupStorageGuide(it.name).fridge_days;
        }  // 常温: the chart only covers fridge/freezer -> only user-set days apply
        if (limit <= 0 || age < limit) continue;
        list.push_back({it.id, it.name, age, static_cast<double>(age) / limit});
    }
    if (list.empty()) return "";
    std::stable_sort(list.begin(), list.end(),
                     [](const Nudge& a, const Nudge& b) { return a.ratio > b.ratio; });
    constexpr size_t kMaxSpoken = 4;
    std::string out = "早上好，冰箱小提醒：";
    for (size_t i = 0; i < list.size() && i < kMaxSpoken; ++i) {
        if (i > 0) out += "，";
        out += list[i].name + "已经放了" + std::to_string(list[i].age) + "天啦";
        if (ids_out) ids_out->push_back(list[i].id);
    }
    if (list.size() > kMaxSpoken) {
        // Not marked: they are named on another morning
        out += "，还有" + std::to_string(list.size() - kMaxSpoken) + "样也放了挺久";
    }
    out += "，有空记得吃掉哦。";
    return out;
}

void FridgeStore::MarkStorageReminded(const std::vector<uint32_t>& ids) {
    if (ids.empty()) return;
    std::lock_guard<std::mutex> lock(mutex_);
    bool changed = false;
    for (auto& it : items_) {
        if (!it.storage_reminded && std::find(ids.begin(), ids.end(), it.id) != ids.end()) {
            it.storage_reminded = true;
            changed = true;
        }
    }
    if (changed) SaveToSpiffsLocked();
}
