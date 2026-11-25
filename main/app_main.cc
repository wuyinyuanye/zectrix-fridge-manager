#include <algorithm>
#include <atomic>
#include <cstdio>
#include <ctime>
#include <functional>
#include <string>
#include <vector>

#include "audio_service.h"
#include "driver/gpio.h"
#include "driver/rtc_io.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "fridge_store.h"
#include "fridge_ui.h"
#include "nfc_shopping.h"
#include "rtc_pcf8563.h"
#include "wifi_web_service.h"
#include "xiaozhi_client.h"
#include "zectrix_board.h"
#include "zectrix_board_config.h"
#include "zectrix_epd.h"
#include "zectrix_nfc.h"

namespace {

constexpr char kTag[] = "fridge_app";

std::string FormatMmSs(int sec) {
    if (sec < 0) sec = 0;
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%02d:%02d", sec / 60, sec % 60);
    return buf;
}

// Conservative e-ink timer label:
// >= 60s -> "剩X分" (refreshes once per minute!)
// <  60s -> "00:SS" (refreshes every 10s, zero full-screen flashing!)
std::string FormatTopBarTimer(int rem_sec) {
    if (rem_sec <= 0) return "00:00";
    if (rem_sec >= 60) {
        return "剩" + std::to_string((rem_sec + 59) / 60) + "分";
    }
    return FormatMmSs(rem_sec);
}

std::string TodayChineseDateWeekday() {
    static constexpr const char* kWday[7] = {"周日", "周一", "周二", "周三", "周四", "周五", "周六"};
    const time_t now = std::time(nullptr);
    struct tm lt = {};
    localtime_r(&now, &lt);
    if (lt.tm_year + 1900 < 2025) return "时间未同步";
    char buf[24];
    std::snprintf(buf, sizeof(buf), "%02d/%02d %s",
                  lt.tm_mon + 1, lt.tm_mday, kWday[lt.tm_wday % 7]);
    return buf;
}

// Matches ZectrixNfc::WriteUriNdef() encoding so we can skip redundant EEPROM writes.
std::vector<uint8_t> BuildExpectedUriNdef(const std::string& uri) {
    uint8_t prefix_code = 0x00;
    std::string suffix = uri;
    if (uri.rfind("https://www.", 0) == 0) {
        prefix_code = 0x02;
        suffix = uri.substr(12);
    } else if (uri.rfind("http://www.", 0) == 0) {
        prefix_code = 0x01;
        suffix = uri.substr(11);
    } else if (uri.rfind("https://", 0) == 0) {
        prefix_code = 0x04;
        suffix = uri.substr(8);
    } else if (uri.rfind("http://", 0) == 0) {
        prefix_code = 0x03;
        suffix = uri.substr(7);
    }
    const size_t payload_len = 1 + suffix.size();
    std::vector<uint8_t> msg;
    if (payload_len <= 0xFF) {
        msg.reserve(4 + payload_len);
        msg.push_back(0xD1);
        msg.push_back(0x01);
        msg.push_back(static_cast<uint8_t>(payload_len));
    } else {
        msg.reserve(7 + payload_len);
        msg.push_back(0xC1);
        msg.push_back(0x01);
        const uint32_t plen = static_cast<uint32_t>(payload_len);
        msg.push_back(static_cast<uint8_t>((plen >> 24) & 0xFF));
        msg.push_back(static_cast<uint8_t>((plen >> 16) & 0xFF));
        msg.push_back(static_cast<uint8_t>((plen >> 8) & 0xFF));
        msg.push_back(static_cast<uint8_t>(plen & 0xFF));
    }
    msg.push_back('U');
    msg.push_back(prefix_code);
    msg.insert(msg.end(), suffix.begin(), suffix.end());
    return msg;
}

// Offline plain-text QR fallback (fits QR Version <= 10 Low ECC = 271 bytes)
std::string BuildOfflineShoppingQrText(const std::vector<ShoppingItem>& list, int* pending_out) {
    std::vector<const ShoppingItem*> pending;
    for (const auto& s : list) {
        if (!s.checked) pending.push_back(&s);
    }
    if (pending_out) *pending_out = static_cast<int>(pending.size());
    const bool use_all = pending.empty();
    std::string out = "采购清单:\n";
    constexpr size_t kMaxBytes = 235;
    int shown = 0;
    const size_t total = use_all ? list.size() : pending.size();
    for (size_t i = 0; i < total; ++i) {
        const ShoppingItem& s = use_all ? list[i] : *pending[i];
        std::string line = "- " + s.name;
        if (!s.qty.empty()) line += " " + s.qty;
        line += "\n";
        if (out.size() + line.size() > kMaxBytes) {
            const int rest = static_cast<int>(total) - shown;
            out += "...等" + std::to_string(rest) + "样";
            break;
        }
        out += line;
        shown++;
    }
    return out;
}

// Only include active-low RTC wake pins (OK, DOWN, PCF8563 RTC_INT) that are currently HIGH.
// NEVER include CHARGE_DETECT_GPIO (GPIO2): when battery is full or charger IC pulses on USB,
// GPIO2 floats/toggles LOW and causes instant self-wakeup out of Page 0 Deep Sleep!
// Returns 0 if ALL three active-low pins are stuck LOW (e.g. jammed button/magnet),
// so EnterDeepSleepStandby switches to ANY_HIGH release-wake instead of wake-looping.
uint64_t ComputeSafeExt1WakeMask(bool ok_high, bool down_high, bool rtc_high) {
    uint64_t mask = 0;
    if (ok_high) mask |= (1ULL << ZECTRIX_BUTTON_OK);
    if (down_high) mask |= (1ULL << ZECTRIX_BUTTON_DOWN);
    if (rtc_high) mask |= (1ULL << ZECTRIX_RTC_INT);
    return mask;
}

class FridgeApp {
public:
    FridgeApp() : audio_(&board_), xiaozhi_(&store_), wifi_web_(&board_, &store_, &audio_, &xiaozhi_) {}

    void Run() {
        gpio_deep_sleep_hold_dis();
        gpio_hold_dis(ZECTRIX_VBAT_LATCH);

        ESP_ERROR_CHECK(board_.Init());
        board_.SetPowerLed(false);

        // Keep NFC powered off during idle; SyncShoppingToNfc() powers it briefly (~50ms) only on list change.
        if (board_.nfc() != nullptr) {
            board_.nfc()->PowerOff();
        }

        zectrix_epd_config_t epd_cfg = {};
        zectrix_epd_get_default_config(&epd_cfg);
        ESP_ERROR_CHECK(zectrix_epd_new(&epd_cfg, &epd_));
        ui_.SetEpd(epd_);

        // Load PCF8563 hardware RTC BEFORE store_.Init() so TodayDayNumber() is valid immediately!
        wifi_web_.LoadSystemTimeFromRtc();
        ESP_ERROR_CHECK(store_.Init());
        if (FridgeStore::IsClockSynced()) {
            store_.ReconcileUnsyncedDates();
        }
        SyncNfcShopUrlIfNeeded(NowMs(), true);

        const esp_sleep_wakeup_cause_t wake_cause = esp_sleep_get_wakeup_cause();
        const uint64_t ext1_status =
            (wake_cause == ESP_SLEEP_WAKEUP_EXT1) ? esp_sleep_get_ext1_wakeup_status() : 0;
        const bool rtc_wakeup = (wake_cause == ESP_SLEEP_WAKEUP_EXT1) &&
                                ((ext1_status & (1ULL << ZECTRIX_RTC_INT)) != 0 ||
                                 (board_.rtc() != nullptr && board_.rtc()->WasAlarmFiredOnBoot()));
        if (board_.rtc() != nullptr) {
            board_.rtc()->DisableAlarm();
            board_.rtc()->ClearAlarmFlag();
        }

        if (!rtc_wakeup) {
            ui_.ShowBootSplash("正在启动极趣小冰箱...");
        }

        xiaozhi_.SetAudio(&audio_);  // play Xiaozhi's spoken replies

        wifi_web_.Start([this](bool switch_recipe, bool switch_cook) {
            if (switch_recipe) pending_tab_.store(static_cast<int>(FridgeTab::kRecipe));
            else if (switch_cook) pending_tab_.store(static_cast<int>(FridgeTab::kCook));
            ui_dirty_.store(true);
        });

        // Morning silent refresh: sync NTP->RTC briefly, re-render 16-gray Page 0, sleep again!
        if (rtc_wakeup) {
            ESP_LOGI(kTag, "morning RTC wakeup: refreshing 16-gray standby poster");
            for (int i = 0; i < 50 && !wifi_web_.is_wifi_connected(); ++i) {
                vTaskDelay(pdMS_TO_TICKS(100));
            }
            if (wifi_web_.is_wifi_connected()) {
                vTaskDelay(pdMS_TO_TICKS(1800));
                wifi_web_.SyncRtcFromSystem();
            }
            EnterDeepSleepStandby(true);
        }

        reminder_check_pending_ = true;  // woken by the user (RTC refresh exited above)
        tab_ = store_.GetConfig().wifi_ssid.empty() ? FridgeTab::kSettings : FridgeTab::kFridge;
        nav_focus_ = (tab_ != FridgeTab::kSettings);  // start on the top menu bar
        selected_ = 0;
        last_activity_ms_ = NowMs();
        RenderCurrentTab(true);

        std::string storage_warn;
        if (store_.ConsumeStorageWarning(&storage_warn)) {
            ShowBriefPopup(false, "", storage_warn, false);
            RenderCurrentTab(false);
        }

        int64_t last_alarm_beep_ms = 0;
        int64_t last_rtc_sync_ms = 0;
        int64_t last_sta_recover_ms = NowMs();
        bool clock_was_synced = FridgeStore::IsClockSynced();
        last_net_wifi_ = wifi_web_.is_wifi_connected();
        last_net_ap_ = wifi_web_.is_ap_mode();
        std::string last_timer_label_drawn;
        int last_timer_bucket = -1;

        // Main loop: ZERO idle screen refreshes! Screen only updates on user input,
        // explicit Web UI change, timer ring, or quiet timer-pill label change.
        while (true) {
            const int64_t now = NowMs();
            wifi_web_.Poll();

            const auto preq = wifi_web_.TakePendingPowerRequest();
            if (preq == WifiWebService::PowerRequest::kReboot) {
                vTaskDelay(pdMS_TO_TICKS(200));  // let HTTP response finish flushing
                RebootDevice();
            } else if (preq == WifiWebService::PowerRequest::kShutdown) {
                vTaskDelay(pdMS_TO_TICKS(200));
                FullClearShutdown();
            }

            const bool clock_now_synced = FridgeStore::IsClockSynced();
            if (clock_now_synced && !clock_was_synced) {
                clock_was_synced = true;
                store_.ReconcileUnsyncedDates();
                wifi_web_.SyncRtcFromSystem();
                last_rtc_sync_ms = now;
            }

            // Periodic SNTP -> PCF8563 RTC sync (background only, zero screen refresh)
            if (wifi_web_.is_wifi_connected() && clock_now_synced &&
                (last_rtc_sync_ms == 0 || now - last_rtc_sync_ms > 300000)) {
                wifi_web_.SyncRtcFromSystem();
                last_rtc_sync_ms = now;
            }

            // Kitchen timers tick
            bool any_ringing = false, any_active = false;
            const bool newly_ringing = store_.TickTimers(now, &any_ringing, &any_active);
            if (newly_ringing || (any_ringing && !alarm_shown_)) {
                alarm_shown_ = true;
                last_activity_ms_ = now;
                board_.SetPowerLed(true);
                const auto ring_timers = store_.GetTimers();
                ui_.ShowAlarmScreen(ring_timers);
                audio_.PlayAlarmBeep();
                last_alarm_beep_ms = now;
                if (newly_ringing && wifi_web_.is_wifi_connected()) {
                    std::string label;
                    for (const auto& t : ring_timers) {
                        if (t.ringing) { label = t.label; break; }
                    }
                    if (label.empty() || label == "厨房计时" || label == "语音计时") {
                        label = "厨房";
                    }
                    // Falls back silently to the beeps if Xiaozhi is busy or offline
                    xiaozhi_.AnnounceAsync(store_.GetConfig(), label + "的计时时间到啦");
                }
            } else if (any_ringing && now - last_alarm_beep_ms >= 2200) {
                board_.SetPowerLed((now / 500) % 2 == 0);
                audio_.PlayAlarmBeep();
                last_alarm_beep_ms = now;
            }

            // Quiet timer-pill update ONLY when displayed label actually changes
            // (>=60s: once per minute "剩X分"; <60s: every 10s "00:50" -> ZERO full-screen flashes!)
            if (!any_ringing && any_active) {
                const auto timers = store_.GetTimers();
                int rem_sec = 0;
                for (const auto& t : timers) {
                    if (t.active) {
                        const int64_t rem_ms = std::max<int64_t>(0, t.end_uptime_ms - now);
                        rem_sec = static_cast<int>((rem_ms + 999) / 1000);
                        break;
                    }
                }
                const int bucket = (rem_sec >= 60) ? ((rem_sec + 59) / 60) : (rem_sec / 10);
                const std::string lbl = FormatTopBarTimer(rem_sec);
                if (bucket != last_timer_bucket && lbl != last_timer_label_drawn) {
                    last_timer_bucket = bucket;
                    last_timer_label_drawn = lbl;
                    UiTopBar bar = BuildTopBar();
                    bar.active_timer_label = lbl;
                    ui_.UpdateTimerPillOnly(bar);
                }
            } else if (!any_active && !last_timer_label_drawn.empty()) {
                last_timer_label_drawn.clear();
                last_timer_bucket = -1;
            }

            // Async refresh ONLY when user explicitly modifies data on Phone Web UI
            const int ptab = pending_tab_.exchange(-1);
            if (ptab >= 0 && ptab < static_cast<int>(FridgeTab::kCount)) {
                tab_ = static_cast<FridgeTab>(ptab);
                nav_focus_ = false;
                selected_ = 0;
                if (tab_ == FridgeTab::kRecipe) {
                    focus_ingredient_.clear();
                    recipe_idx_ = 0;
                    cook_step_idx_ = 0;
                }
            }
            if (!any_ringing && ui_dirty_.exchange(false)) {
                last_activity_ms_ = now;
                RenderCurrentTab(false);
            }

            // Keep NFC tag synced (custom static URL if configured, otherwise http://<current-IP>/#shop)
            SyncNfcShopUrlIfNeeded(now, false);

            // On Page 5 only: update QR & IP ONCE when Wi-Fi finishes connecting or switches AP/STA
            const bool cur_wifi = wifi_web_.is_wifi_connected();
            const bool cur_ap = wifi_web_.is_ap_mode();
            if (cur_wifi != last_net_wifi_ || cur_ap != last_net_ap_) {
                last_net_wifi_ = cur_wifi;
                last_net_ap_ = cur_ap;
                last_sta_recover_ms = now;
                if (!any_ringing && tab_ == FridgeTab::kSettings) {
                    RenderCurrentTab(false);
                }
            } else if (cur_ap && (now - last_sta_recover_ms) >= 180000) {
                last_sta_recover_ms = now;
                wifi_web_.TryAutoRecoverSta();
            }

            // Dual-mode power policy: auto-sleep ONLY on battery when idle & no active timers
            const AppConfig cfg = store_.GetConfig();
            const ZectrixPowerSnapshot pwr = board_.ReadPowerSnapshot();
            if (!any_active && !any_ringing && !pwr.charge.power_present &&
                cfg.sleep_minutes > 0 &&
                (now - last_activity_ms_) >= static_cast<int64_t>(cfg.sleep_minutes) * 60000) {
                EnterDeepSleepStandby();
            }

            if (reminder_check_pending_ && !any_ringing) {
                MaybeAnnounceMorningReminder();
            }

            ZectrixButtonEvent ev;
            if (!board_.WaitButton(&ev, pdMS_TO_TICKS(100))) continue;
            last_activity_ms_ = NowMs();
            reminder_check_pending_ = true;  // a key press also counts as waking the device

            // Any key stops a spoken reply / announcement that is still playing
            if (audio_.IsTtsActive() || xiaozhi_.is_announcing()) {
                xiaozhi_.CancelAnnounce();
                audio_.TtsStop();
            }
            ESP_LOGI(kTag, "Button event: btn=%d act=%d (tab=%d sel=%d)",
                     static_cast<int>(ev.button), static_cast<int>(ev.action),
                     static_cast<int>(tab_), selected_);

            // Any button silences ringing kitchen alarm
            if (any_ringing || alarm_shown_) {
                store_.StopAllRinging();
                alarm_shown_ = false;
                board_.SetPowerLed(false);
                board_.DrainButtons();
                RenderCurrentTab(true);
                continue;
            }

            // Fast continuous page flip when holding DOWN inside multi-page lists
            if (ev.button == ZectrixButton::kDown && ev.action == ZectrixButtonAction::kRepeat) {
                HandleDownPageFlip();
                // Drain any queued repeat so lifting finger stops immediately with zero overshoot
                board_.DrainButtons();
                continue;
            }

            // Global: DOWN 3s long-press -> enter Page 0 16-gray standby sleep
            if (ev.button == ZectrixButton::kDown && ev.action == ZectrixButtonAction::kLongPress) {
                RequestSleepOrConfirm(false);
                continue;
            }

            // Global: UP hold (600ms) -> jump straight back to the top menu bar
            if (ev.button == ZectrixButton::kUp && ev.action == ZectrixButtonAction::kLongPress) {
                EnterNavFocus();
                board_.DrainButtons();
                continue;
            }

            // Global: OK hold (350ms) -> True PTT Voice Recording with clear center result popup!
            if (ev.button == ZectrixButton::kOk && ev.action == ZectrixButtonAction::kLongPress) {
                RunPttVoiceFlow();
                continue;
            }

            if (ev.action != ZectrixButtonAction::kClick) continue;

            // Coalesce rapid UP or DOWN taps so e-paper refresh lag NEVER replays leftover
            // queued clicks after the screen has already reached the target page/item!
            int repeat_steps = 1;
            bool requested_sleep = false;
            if (ev.button == ZectrixButton::kUp || ev.button == ZectrixButton::kDown) {
                ZectrixButtonEvent next_ev;
                while (board_.WaitButton(&next_ev, pdMS_TO_TICKS(150))) {
                    if (next_ev.action == ZectrixButtonAction::kLongPress) {
                        if (next_ev.button == ZectrixButton::kDown) {
                            RequestSleepOrConfirm(false);
                            requested_sleep = true;
                        } else if (next_ev.button == ZectrixButton::kUp) {
                            EnterNavFocus();
                            requested_sleep = true;  // consumed: skip the queued click
                        }
                        break;
                    }
                    if (next_ev.action == ZectrixButtonAction::kClick && next_ev.button == ev.button) {
                        repeat_steps++;
                    }
                }
            }
            if (requested_sleep) continue;
            HandleFlatClick(ev.button, repeat_steps);
            // Discard any accidental button taps made blindly while EPD SPI refresh was busy
            board_.DrainButtons();
        }
    }

private:
    static int64_t NowMs() { return esp_timer_get_time() / 1000; }

    // Offload blocking network/AI calls to a worker task so the main loop keeps
    // ticking kitchen timers and ringing alarms on time (never blocked up to 36s!).
    template <typename Fn>
    auto RunWithTimerTicking(const char* task_name, uint32_t stack_bytes,
                             bool allow_silence_click, Fn&& fn,
                             const std::function<void()>& on_tick = nullptr) -> decltype(fn()) {
        using Ret = decltype(fn());
        struct WorkerCtx {
            Fn* fn;
            Ret result{};
            std::atomic<bool> done{false};
        };
        WorkerCtx ctx{&fn, Ret{}, false};
        auto entry = [](void* arg) {
            auto* c = static_cast<WorkerCtx*>(arg);
            c->result = (*c->fn)();
            c->done.store(true, std::memory_order_release);
            vTaskDelete(nullptr);
        };
        if (xTaskCreate(entry, task_name, stack_bytes, &ctx, 4, nullptr) != pdPASS) {
            return fn();
        }
        int64_t last_beep_ms = 0;
        while (!ctx.done.load(std::memory_order_acquire)) {
            const int64_t now = NowMs();
            bool any_ringing = false, any_active = false;
            const bool newly = store_.TickTimers(now, &any_ringing, &any_active);
            if (newly || (any_ringing && (last_beep_ms == 0 || now - last_beep_ms >= 2200))) {
                board_.SetPowerLed(true);
                audio_.PlayAlarmBeep();
                last_beep_ms = now;
            }
            if (allow_silence_click && any_ringing) {
                ZectrixButtonEvent ev;
                if (board_.WaitButton(&ev, 0) && ev.action == ZectrixButtonAction::kClick) {
                    store_.StopAllRinging();
                    board_.SetPowerLed(false);
                    alarm_shown_ = false;
                }
            }
            if (on_tick) on_tick();
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        return std::move(ctx.result);
    }

    UiTopBar BuildTopBar() {
        UiTopBar bar;
        bar.nav_focus = nav_focus_;
        bar.hm_time = FridgeStore::NowHmString();
        bar.wifi_ok = wifi_web_.is_wifi_connected();
        bar.ap_mode = wifi_web_.is_ap_mode();
        const ZectrixPowerSnapshot pwr = board_.ReadPowerSnapshot();
        bar.battery_pct = pwr.battery_valid ? pwr.battery_percent : 0;
        bar.charging = pwr.charge.charging;

        const auto timers = store_.GetTimers();
        const int64_t now_ms = NowMs();
        for (const auto& t : timers) {
            if (t.active) {
                const int64_t rem_ms = std::max<int64_t>(0, t.end_uptime_ms - now_ms);
                const int rem = static_cast<int>((rem_ms + 999) / 1000);
                bar.active_timer_label = FormatTopBarTimer(rem);
                break;
            }
        }
        return bar;
    }

    void RenderCurrentTab(bool force_full) {
        SyncNfcShopUrlIfNeeded(NowMs(), false);
        const UiTopBar bar = BuildTopBar();
        switch (tab_) {
            case FridgeTab::kFridge: {
                int c_all = 0, c_exp = 0, c_fr = 0, c_fz = 0;
                const auto items = store_.GetItemsFiltered(ZoneFilter::kAll, &c_all, &c_exp, &c_fr, &c_fz);
                selected_ = items.empty() ? 0 : std::clamp(selected_, 0, static_cast<int>(items.size()) - 1);
                board_.SetDownRepeatEnabled(!nav_focus_ && items.size() > FridgeUi::kFridgePageSize);
                ui_.RenderFridgePage(bar, c_all, c_fr, c_fz, items, nav_focus_ ? -1 : selected_, force_full);
                break;
            }
            case FridgeTab::kRecipe: {
                board_.SetDownRepeatEnabled(false);
                const auto recipes = store_.GetMatchedRecipesSorted(focus_ingredient_);
                recipe_idx_ = recipes.empty() ? 0 : std::clamp(recipe_idx_, 0, static_cast<int>(recipes.size()) - 1);
                selected_ = std::clamp(selected_, 0, 2);
                ui_.RenderRecipePage(bar, recipes, recipe_idx_, nav_focus_ ? -1 : selected_, force_full);
                break;
            }
            case FridgeTab::kCook: {
                board_.SetDownRepeatEnabled(false);
                const auto recipes = store_.GetMatchedRecipesSorted(focus_ingredient_);
                const RecipeMatchView* cur = nullptr;
                if (!recipes.empty()) {
                    recipe_idx_ = std::clamp(recipe_idx_, 0, static_cast<int>(recipes.size()) - 1);
                    cur = &recipes[recipe_idx_];
                    const int ns = static_cast<int>(cur->recipe.steps.size());
                    cook_step_idx_ = (ns > 0) ? std::clamp(cook_step_idx_, 0, ns - 1) : 0;
                } else {
                    cook_step_idx_ = 0;
                }
                ui_.RenderCookPage(bar, cur, cook_step_idx_, store_.GetTimers(), force_full);
                break;
            }
            case FridgeTab::kShopping: {
                const auto list = store_.GetShoppingList();
                selected_ = list.empty() ? 0 : std::clamp(selected_, 0, static_cast<int>(list.size()) + 1);
                board_.SetDownRepeatEnabled(!nav_focus_ && list.size() > 4);
                ui_.RenderShoppingPage(bar, list, nav_focus_ ? -1 : selected_, force_full,
                                       board_.nfc() != nullptr);
                break;
            }
            case FridgeTab::kSettings: {
                board_.SetDownRepeatEnabled(false);
                selected_ = std::clamp(selected_, 0, 3);
                const auto cfg = store_.GetConfig();
                const bool ap = wifi_web_.is_ap_mode();
                const std::string qr = ap ? wifi_web_.ap_wifi_qr_payload() : wifi_web_.access_url();
                const std::string cap = ap ? "扫码连配网热点" : "扫码打开管理页";
                ui_.RenderSettingsPage(bar, cfg, wifi_web_.ip_address(),
                                       qr, cap, wifi_web_.ap_password(),
                                       board_.ReadPowerSnapshot(), nav_focus_ ? -1 : selected_, force_full);
                break;
            }
            default:
                break;
        }
    }

    // ========================================================================
    // FLAT 3-BUTTON MODEL (ZERO nested focus layers!)
    //   UP   click = ALWAYS switch to next page (1 click from ANY position!)
    //   DOWN click = ALWAYS move to next item / next button / next cooking step
    //   OK   click = ALWAYS execute the single highlighted item
    // ========================================================================
    // Focus model:
    //   MENU (top tab bar): UP/DOWN pick a page (wrapping), OK enters that page.
    //   PAGE: DOWN moves down (wrapping); UP moves up and, from the first item, returns
    //         to the MENU. Holding UP returns to the MENU from anywhere.
    // Morning storage reminder ("X已经放了N天啦", USDA storage chart, each item once): spoken once a day, on the first time the user wakes / uses
    // the device between 06:00 and 12:00 (the automatic morning screen refresh stays silent).
    void MaybeAnnounceMorningReminder() {
        if (!FridgeStore::IsClockSynced()) return;      // need the real date & hour
        if (!wifi_web_.is_wifi_connected()) return;     // keep pending until Wi-Fi is up
        if (xiaozhi_.is_announcing() || xiaozhi_.is_ptt_active()) return;
        reminder_check_pending_ = false;

        const time_t now = std::time(nullptr);
        struct tm lt = {};
        localtime_r(&now, &lt);
        if (lt.tm_hour < 6 || lt.tm_hour >= 12) return;

        const int32_t today = FridgeStore::TodayDayNumber();
        if (today <= 0 || store_.GetLastReminderDay() == today) return;
        if (reminder_attempts_day_ != today) {
            reminder_attempts_day_ = today;
            reminder_attempts_ = 0;
        }
        if (reminder_attempts_ >= 3) return;  // don't keep retrying a failing network all morning

        std::vector<uint32_t> ids;
        const std::string notice = store_.BuildStorageReminderText(&ids);
        if (notice.empty()) {
            store_.SetLastReminderDay(today);  // nothing to mention: today's first wake is done
            return;
        }
        reminder_attempts_++;
        ESP_LOGI(kTag, "morning reminder: %s", notice.c_str());
        xiaozhi_.AnnounceAsync(store_.GetConfig(), notice, [this, today, ids](bool spoken) {
            if (!spoken) return;  // retried on a later wake this morning
            store_.SetLastReminderDay(today);
            store_.MarkStorageReminded(ids);  // each item is mentioned only once
        });
    }

    void EnterNavFocus() {
        nav_focus_ = true;
        selected_ = 0;
        RenderCurrentTab(false);
    }

    // Hold DOWN in multi-page lists: smoothly flip whole pages continuously while held
    void HandleDownPageFlip() {
        if (nav_focus_) return;
        if (tab_ == FridgeTab::kFridge) {
            constexpr int kPerPage = FridgeUi::kFridgePageSize;
            const auto items = store_.GetItemsFiltered(ZoneFilter::kAll);
            const int n = static_cast<int>(items.size());
            if (n <= kPerPage) return;
            const int total_pages = (n + kPerPage - 1) / kPerPage;
            const int next_page = ((std::clamp(selected_, 0, n - 1) / kPerPage) + 1) % total_pages;
            selected_ = next_page * kPerPage;
            RenderCurrentTab(false);
        } else if (tab_ == FridgeTab::kShopping) {
            const auto list = store_.GetShoppingList();
            const int n = static_cast<int>(list.size());
            if (n <= 4) return;
            const int total = n + 2;
            selected_ = (selected_ + 4) % total;
            RenderCurrentTab(false);
        }
    }

    void HandleFlatClick(ZectrixButton btn, int steps = 1) {
        steps = std::max(1, steps);
        const int n_tabs = static_cast<int>(FridgeTab::kCount);

        if (nav_focus_) {
            if (btn == ZectrixButton::kUp || btn == ZectrixButton::kDown) {
                const int dir = (btn == ZectrixButton::kUp) ? -1 : 1;
                const int t = static_cast<int>(tab_) + dir * steps;
                tab_ = static_cast<FridgeTab>(((t % n_tabs) + n_tabs) % n_tabs);
                selected_ = 0;
                RenderCurrentTab(false);
            } else if (btn == ZectrixButton::kOk) {
                nav_focus_ = false;
                selected_ = 0;
                RenderCurrentTab(false);
            }
            return;
        }

        // Inside a page. Returns false when UP walked off the top (focus went to the MENU).
        auto move = [&](int* idx, int n) -> bool {
            if (btn == ZectrixButton::kUp) {
                if (*idx - steps < 0) {
                    EnterNavFocus();
                    return false;
                }
                *idx -= steps;
            } else {
                *idx = (n <= 0) ? 0 : (*idx + steps) % n;
            }
            return true;
        };
        const bool is_nav_key = (btn == ZectrixButton::kUp || btn == ZectrixButton::kDown);

        // ---- Page 1: Fridge Inventory ----
        if (tab_ == FridgeTab::kFridge) {
            const auto items = store_.GetItemsFiltered(ZoneFilter::kAll);
            if (items.empty()) {
                if (btn == ZectrixButton::kUp) EnterNavFocus();
                else RenderCurrentTab(false);
                return;
            }
            const int n = static_cast<int>(items.size());
            if (is_nav_key) {
                if (move(&selected_, n)) RenderCurrentTab(false);
            } else if (btn == ZectrixButton::kOk) {
                RunItemPopup(items[std::clamp(selected_, 0, n - 1)]);
            }
            return;
        }

        // ---- Page 2: Interlocking Recipe (3 bottom buttons) ----
        if (tab_ == FridgeTab::kRecipe) {
            const auto recipes = store_.GetMatchedRecipesSorted(focus_ingredient_);
            if (recipes.empty()) {
                if (btn == ZectrixButton::kUp) EnterNavFocus();
                return;
            }
            if (is_nav_key) {
                if (move(&selected_, 3)) RenderCurrentTab(false);
            } else if (btn == ZectrixButton::kOk) {
                const int ri = std::clamp(recipe_idx_, 0, static_cast<int>(recipes.size()) - 1);
                const auto& mv = recipes[ri];
                if (selected_ == 0) {
                    cook_step_idx_ = 0;
                    tab_ = FridgeTab::kCook;
                    nav_focus_ = false;
                    selected_ = 0;
                    RenderCurrentTab(false);
                } else if (selected_ == 1) {
                    if (mv.missing_count > 0) {
                        const int added = store_.AddMissingToShopping(mv);
                        if (added == mv.missing_count) {
                            audio_.PlayChimeOk();
                            ShowBriefPopup(true, "缺料一键加采购",
                                           "已将 " + std::to_string(added) + " 样缺料加入采购清单", false);
                        } else {
                            audio_.PlayChimeError();
                            ShowBriefPopup(false, "缺料一键加采购",
                                           "已加入 " + std::to_string(added) + " 样，采购清单已满(最多80样)", false);
                        }
                    } else {
                        ShowBriefPopup(true, "原料检查", "这道菜所需食材冰箱里都齐啦!", false);
                    }
                    RenderCurrentTab(false);
                } else {
                    recipe_idx_ = (ri + 1) % static_cast<int>(recipes.size());
                    cook_step_idx_ = 0;
                    RenderCurrentTab(false);
                }
            }
            return;
        }

        // ---- Page 3: Cooking Guide (DOWN = next step, OK = start timer / finish) ----
        if (tab_ == FridgeTab::kCook) {
            const auto recipes = store_.GetMatchedRecipesSorted(focus_ingredient_);
            const RecipeMatchView* cur = recipes.empty()
                                             ? nullptr
                                             : &recipes[std::clamp(recipe_idx_, 0, static_cast<int>(recipes.size()) - 1)];
            const int n_steps = (cur != nullptr) ? static_cast<int>(cur->recipe.steps.size()) : 0;
            if (n_steps == 0) {
                if (btn == ZectrixButton::kUp) EnterNavFocus();
                return;
            }

            if (is_nav_key) {
                if (move(&cook_step_idx_, n_steps)) RenderCurrentTab(false);
            } else if (btn == ZectrixButton::kOk) {
                const int st_i = std::clamp(cook_step_idx_, 0, n_steps - 1);
                const auto& st = cur->recipe.steps[st_i];
                uint32_t running_id = 0;
                for (const auto& t : store_.GetTimers()) {
                    if (t.active && t.label == cur->recipe.title) { running_id = t.id; break; }
                }
                if (st_i == n_steps - 1) {
                    RunCookDonePopup(*cur);
                } else if (running_id != 0) {
                    // Button shows "计时中 · 按OK停止": OK cancels this recipe's step timer
                    store_.StopTimer(running_id);
                    audio_.PlayChimeStop();
                    RenderCurrentTab(false);
                } else {
                    const int mins = (st.timer_min > 0) ? st.timer_min : 5;
                    store_.StartTimer(cur->recipe.title, mins);
                    audio_.PlayChimeOk();
                    RenderCurrentTab(false);
                }
            }
            return;
        }

        // ---- Page 4: Shopping List ----
        if (tab_ == FridgeTab::kShopping) {
            const auto list = store_.GetShoppingList();
            if (list.empty()) {
                if (btn == ZectrixButton::kUp) EnterNavFocus();
                else RenderCurrentTab(false);
                return;
            }
            const int n = static_cast<int>(list.size());
            const int total = n + 2;
            if (is_nav_key) {
                if (move(&selected_, total)) RenderCurrentTab(false);
            } else if (btn == ZectrixButton::kOk) {
                if (selected_ < n) {
                    store_.ToggleShoppingItem(list[selected_].id);
                    RenderCurrentTab(false);
                } else if (selected_ == n) {
                    int skipped_full = 0;
                    const int moved = store_.CommitCheckedShoppingToFridge(&skipped_full);
                    if (moved > 0 && skipped_full == 0) {
                        audio_.PlayChimeOk();
                        selected_ = 0;
                        ShowBriefPopup(true, "采购一键入库",
                                       "已将 " + std::to_string(moved) + " 样食材放进冰箱!", false);
                    } else if (skipped_full > 0) {
                        audio_.PlayChimeError();
                        selected_ = 0;
                        ShowBriefPopup(false, "冰箱库存已满",
                                       "已入库 " + std::to_string(moved) + " 样，另有 " +
                                           std::to_string(skipped_full) + " 样因满80样保留在采购单",
                                       false);
                    } else {
                        ShowBriefPopup(false, "", "请先用上下键选物品、按 OK 勾选已买项", false);
                    }
                    RenderCurrentTab(false);
                } else {
                    RunShoppingQrPopup(list);
                }
            }
            return;
        }

        // ---- Page 5: Settings (4 clear buttons, NO NFC!) ----
        if (tab_ == FridgeTab::kSettings) {
            if (is_nav_key) {
                if (move(&selected_, 4)) RenderCurrentTab(false);
            } else if (btn == ZectrixButton::kOk) {
                if (selected_ == 0) {
                    RunVoiceHelpPopup();
                } else if (selected_ == 1) {
                    wifi_web_.ReconnectStaOrAp(!wifi_web_.is_ap_mode(), true);
                    last_net_wifi_ = wifi_web_.is_wifi_connected();
                    last_net_ap_ = wifi_web_.is_ap_mode();
                    RenderCurrentTab(false);
                } else if (selected_ == 2) {
                    RequestPowerOrConfirm(PowerAction::kSleep);
                } else {
                    RunPowerMenuPopup();
                }
            }
        }
    }

    // Streamlined 3-action Item Popup (UP = ALWAYS close immediately!)
    void RunItemPopup(const FridgeItem& item) {
        board_.SetDownRepeatEnabled(false);
        int act = 0;
        const bool has_local = store_.HasRecipeForIngredient(item.name);
        ui_.RenderItemPopup(item, act, has_local);
        while (true) {
            ZectrixButtonEvent ev;
            if (!board_.WaitButton(&ev, pdMS_TO_TICKS(15000))) break;
            if (ev.button == ZectrixButton::kDown && ev.action == ZectrixButtonAction::kLongPress) {
                RequestSleepOrConfirm(false);
                return;
            }
            if (ev.action != ZectrixButtonAction::kClick) continue;
            if (ev.button == ZectrixButton::kUp) {
                break;  // 1 click UP = immediate exit!
            } else if (ev.button == ZectrixButton::kDown) {
                act = (act + 1) % 3;
                ui_.RenderItemPopup(item, act, has_local);
            } else if (ev.button == ZectrixButton::kOk) {
                if (act == 0) {
                    if (store_.HasRecipeForIngredient(item.name)) {
                        focus_ingredient_ = item.name;
                        recipe_idx_ = 0;
                        cook_step_idx_ = 0;
                        tab_ = FridgeTab::kRecipe;
                        nav_focus_ = false;
                        selected_ = 0;
                        RenderCurrentTab(false);
                        return;
                    }
                    if (!wifi_web_.is_wifi_connected()) {
                        audio_.PlayChimeError();
                        ShowBriefPopup(false, "",
                                       "暂无“" + item.name + "”现成食谱，连上 Wi-Fi 后可自动定制",
                                       false);
                        break;
                    }
                    ui_.RenderLoadingPopup(
                        "正在定制食谱...",
                        "正在为“" + item.name + "”挑一道地道的家常菜，请稍候...");
                    const std::string prompt =
                        "请推荐一道以" + item.name + "为主料的经典中式家常菜。"
                        "要求：必须是大家熟知、真实存在的传统菜，菜名和做法按传统做法，"
                        "例如排骨可以做红烧排骨、糖醋排骨、豉汁蒸排骨。"
                        "不要为了用掉冰箱里的食材而把不相干的食材硬凑进同一道菜，"
                        "不要自创菜名；配料按这道菜的正宗做法列，冰箱里没有的照样列出。"
                        "写3到4步，每步22字以内，然后调用fridge_set_recipe工具推送。";
                    const AppConfig cfg = store_.GetConfig();
                    const XiaozhiVoiceResult r = RunWithTimerTicking(
                        "dev_recipe", 16384, true, [&]() {
                            return xiaozhi_.SendTextCommand(cfg, prompt, item.name);
                        });
                    if (r.ok && (r.recipe_updated || store_.HasRecipeForIngredient(item.name))) {
                        store_.RecordAiLog("[定制菜谱] " + item.name, r.reply, r.latency_ms, true);
                        audio_.PlayChimeOk();
                        focus_ingredient_ = item.name;
                        recipe_idx_ = 0;
                        cook_step_idx_ = 0;
                        tab_ = FridgeTab::kRecipe;
                        nav_focus_ = false;
                        selected_ = 0;
                        RenderCurrentTab(false);
                        return;
                    }
                    audio_.PlayChimeError();
                    store_.RecordAiLog("[定制菜谱] " + item.name,
                                       r.error.empty() ? r.reply : r.error, r.latency_ms, false);
                    ShowBriefPopup(false, "",
                                   r.error.empty() ? "AI 未返回完整菜谱步骤，请再试一次" : r.error,
                                   false);
                    break;
                } else if (act == 1) {
                    store_.RemoveItemById(item.id);
                    audio_.PlayChimeOk();
                } else {
                    if (store_.MoveItemToShopping(item.id)) {
                        audio_.PlayChimeOk();
                    } else {
                        audio_.PlayChimeError();
                        ShowBriefPopup(false, "", "采购清单已满(最多80样)，请先清理", false);
                    }
                }
                break;
            }
        }
        RenderCurrentTab(false);
    }

    void RunCookDonePopup(const RecipeMatchView& mv) {
        int choice = 0;
        ui_.RenderCookDonePopup(mv, choice);
        while (true) {
            ZectrixButtonEvent ev;
            if (!board_.WaitButton(&ev, pdMS_TO_TICKS(20000))) break;
            if (ev.button == ZectrixButton::kDown && ev.action == ZectrixButtonAction::kLongPress) {
                RequestSleepOrConfirm(false);
                return;
            }
            if (ev.action != ZectrixButtonAction::kClick) continue;
            if (ev.button == ZectrixButton::kUp) {
                break;  // 1 click UP = immediate exit!
            } else if (ev.button == ZectrixButton::kDown) {
                choice = (choice + 1) % 2;
                ui_.RenderCookDonePopup(mv, choice);
            } else if (ev.button == ZectrixButton::kOk) {
                if (choice == 0) {
                    const int n = store_.DeductCookedIngredients(mv);
                    audio_.PlayChimeOk();
                    ShowBriefPopup(true, "开饭啦!",
                                   "已自动扣减 " + std::to_string(n) + " 样用掉的食材", false);
                    tab_ = FridgeTab::kFridge;
                    nav_focus_ = false;
                    selected_ = 0;
                    RenderCurrentTab(false);
                    return;
                }
                break;
            }
        }
        RenderCurrentTab(false);
    }

    void RunActivationPopup(const AppConfig& cfg, const XiaozhiOtaInfo& initial_ota) {
        XiaozhiOtaInfo ota = initial_ota;
        std::string status = "输入 6 位码后按 OK 完成验证";
        ui_.RenderActivationPopup(ota.activation_code, status);
        while (true) {
            ZectrixButtonEvent ev;
            if (!board_.WaitButton(&ev, pdMS_TO_TICKS(60000))) break;
            if (ev.button == ZectrixButton::kDown && ev.action == ZectrixButtonAction::kLongPress) {
                RequestSleepOrConfirm(false);
                return;
            }
            if (ev.action != ZectrixButtonAction::kClick) continue;
            if (ev.button == ZectrixButton::kUp) break;
            if (ev.button == ZectrixButton::kOk) {
                std::string err;
                if (xiaozhi_.PollActivate(cfg, ota.activation_challenge, &err)) {
                    audio_.PlayChimeOk();
                    ShowBriefPopup(true, "设备绑定成功!", "现在可以按住 OK 免费语音控冰箱啦", false);
                    break;
                }
                // Re-check OTA in case server already bound device
                XiaozhiOtaInfo refreshed;
                if (xiaozhi_.CheckOta(cfg, &refreshed, true) && !refreshed.needs_activation) {
                    audio_.PlayChimeOk();
                    ShowBriefPopup(true, "设备绑定成功!", "现在可以按住 OK 免费语音控冰箱啦", false);
                    break;
                }
                if (!refreshed.activation_code.empty()) ota = refreshed;
                audio_.PlayChimeError();
                status = err.empty() ? "尚未绑定成功，请确认输入后再按 OK" : err;
                ui_.RenderActivationPopup(ota.activation_code, status);
            }
        }
        RenderCurrentTab(false);
    }

    void SyncNfcShopUrlIfNeeded(int64_t now_ms, bool force = false) {
        ZectrixNfc* nfc = board_.nfc();
        if (nfc == nullptr) return;
        const AppConfig cfg = store_.GetConfig();
        std::string target;
        bool use_static_offline = false;
        if (!cfg.nfc_shop_url.empty()) {
            target = nfc_shopping::BuildNfcShoppingUrl(cfg.nfc_shop_url, store_.GetShoppingList(),
                                                       nfc->GetUserDataCapacity());
            use_static_offline = true;
        } else if (wifi_web_.is_wifi_connected()) {
            target = wifi_web_.shop_url();
        }
        if (target.empty() || (!force && target == last_nfc_url_)) return;
        if (!force && last_nfc_sync_attempt_ms_ != 0 && (now_ms - last_nfc_sync_attempt_ms_) < 3000) {
            return;
        }
        last_nfc_sync_attempt_ms_ = now_ms;

        if (!nfc->PowerOn()) {
            nfc->PowerOff();
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(15));

        const std::vector<uint8_t> expected = BuildExpectedUriNdef(target);
        std::vector<uint8_t> existing;
        const bool already_matches = (!expected.empty() && nfc->ReadNdef(&existing) == ESP_OK &&
                                      existing == expected);
        if (already_matches) {
            ESP_LOGI(kTag, "NFC shop URL already up-to-date: %s", target.c_str());
            last_nfc_url_ = target;
        } else {
            esp_err_t werr = nfc->WriteUriNdef(target);
            if (werr != ESP_OK) {
                vTaskDelay(pdMS_TO_TICKS(60));
                werr = nfc->WriteUriNdef(target);
            }
            ESP_LOGI(kTag, "NFC shop URL write (%s, %u bytes): %s",
                     use_static_offline ? "offline-static" : "lan",
                     static_cast<unsigned>(target.size()), esp_err_to_name(werr));
            if (werr == ESP_OK) {
                last_nfc_url_ = target;
            }
        }
        nfc->PowerOff();
    }

    void RunShoppingQrPopup(const std::vector<ShoppingItem>& list) {
        board_.SetDownRepeatEnabled(false);
        const bool online = wifi_web_.is_wifi_connected();
        int pending = 0;
        const std::string offline_text = BuildOfflineShoppingQrText(list, &pending);
        const std::string qr_payload = online ? wifi_web_.shop_url() : offline_text;
        const bool has_nfc = (board_.nfc() != nullptr);
        ui_.RenderShoppingQrPopup(qr_payload, online, has_nfc, wifi_web_.ip_address(),
                                  pending > 0 ? pending : static_cast<int>(list.size()));
        ZectrixButtonEvent ev;
        while (board_.WaitButton(&ev, pdMS_TO_TICKS(45000))) {
            if (ev.button == ZectrixButton::kDown && ev.action == ZectrixButtonAction::kLongPress) {
                RequestSleepOrConfirm(false);
                return;
            }
            if (ev.action == ZectrixButtonAction::kClick) break;
        }
        board_.DrainButtons();
        RenderCurrentTab(false);
    }

    void RunVoiceHelpPopup() {
        ui_.RenderVoiceHelpPopup();
        ZectrixButtonEvent ev;
        while (board_.WaitButton(&ev, pdMS_TO_TICKS(25000))) {
            if (ev.button == ZectrixButton::kDown && ev.action == ZectrixButtonAction::kLongPress) {
                RequestSleepOrConfirm(false);
                return;
            }
            if (ev.action != ZectrixButtonAction::kClick) continue;
            if (ev.button == ZectrixButton::kOk) {
                if (!wifi_web_.is_wifi_connected()) {
                    audio_.PlayChimeError();
                    ShowBriefPopup(false, "", "请先连接家庭 2.4G Wi-Fi 再检查语音服务", false);
                } else {
                    const AppConfig cfg = store_.GetConfig();
                    XiaozhiOtaInfo ota;
                    int ms = 0;
                    std::string err;
                    const bool test_ok = RunWithTimerTicking("dev_test", 16384, true, [&]() {
                        return xiaozhi_.TestConnection(cfg, &ota, &ms, &err);
                    });
                    if (test_ok) {
                        audio_.PlayChimeOk();
                        ShowBriefPopup(true, "语音服务已就绪",
                                       "小智 MCP 握手成功 (" + std::to_string(ms) + "ms)，随时按住 OK 说话", false);
                    } else if (ota.needs_activation) {
                        RunActivationPopup(cfg, ota);
                        return;
                    } else {
                        audio_.PlayChimeError();
                        ShowBriefPopup(false, "", err.empty() ? "语音服务连接失败" : err, false);
                    }
                }
                break;
            }
            break;
        }
        RenderCurrentTab(false);
    }

    // Returns true if user pressed UP to undo!
    bool ShowBriefPopup(bool ok, const std::string& heard,
                        const std::string& detail, bool can_undo,
                        bool already_rendered = false) {
        if (!already_rendered) ui_.RenderVoiceResultPopup(ok, heard, detail, can_undo);
        const int timeout_ms = ok ? 5000 : 12000;
        ZectrixButtonEvent ev;
        while (board_.WaitButton(&ev, pdMS_TO_TICKS(timeout_ms))) {
            if (ev.button == ZectrixButton::kDown && ev.action == ZectrixButtonAction::kLongPress) {
                RequestSleepOrConfirm(false);
                return false;
            }
            if (ev.action != ZectrixButtonAction::kClick) continue;
            if (can_undo && ev.button == ZectrixButton::kUp) {
                return true;  // Undo requested!
            }
            break;
        }
        return false;
    }

    enum class PowerAction : uint8_t {
        kSleep = 0,
        kReboot = 1,
        kShutdown = 2,
    };

    [[noreturn]] void ExecutePowerAction(PowerAction action) {
        if (action == PowerAction::kReboot) RebootDevice();
        if (action == PowerAction::kShutdown) FullClearShutdown();
        EnterDeepSleepStandby();
    }

    // Warn before entering Deep Sleep / Reboot / Shutdown if a kitchen countdown is actively running!
    void RequestSleepOrConfirm(bool full_shutdown) {
        RequestPowerOrConfirm(full_shutdown ? PowerAction::kShutdown : PowerAction::kSleep);
    }

    void RequestPowerOrConfirm(PowerAction action) {
        const auto timers = store_.GetTimers();
        const int64_t now_ms = NowMs();
        std::string active_desc;
        for (const auto& t : timers) {
            if (t.active || t.ringing) {
                const int64_t rem_ms = std::max<int64_t>(0, t.end_uptime_ms - now_ms);
                const int rem = t.active ? static_cast<int>((rem_ms + 999) / 1000) : 0;
                active_desc = t.label + " " + FormatTopBarTimer(rem);
                break;
            }
        }
        if (active_desc.empty()) {
            ExecutePowerAction(action);
        }

        const char* act_label =
            (action == PowerAction::kReboot) ? "重启"
                                             : ((action == PowerAction::kShutdown) ? "关机" : "休眠");
        audio_.PlayChimeError();
        int choice = 0;
        ui_.RenderSleepTimerConfirmPopup(active_desc, choice, act_label);
        while (true) {
            ZectrixButtonEvent ev;
            if (!board_.WaitButton(&ev, pdMS_TO_TICKS(15000))) break;
            if (ev.action != ZectrixButtonAction::kClick) continue;
            if (ev.button == ZectrixButton::kUp) break;
            if (ev.button == ZectrixButton::kDown) {
                choice = (choice + 1) % 2;
                ui_.RenderSleepTimerConfirmPopup(active_desc, choice, act_label);
            } else if (ev.button == ZectrixButton::kOk) {
                if (choice == 1) {
                    store_.StopAllTimers();
                    ExecutePowerAction(action);
                }
                break;
            }
        }
        board_.DrainButtons();
        RenderCurrentTab(false);
    }

    void RunPowerMenuPopup() {
        int choice = 0;
        ui_.RenderPowerMenuPopup(choice);
        while (true) {
            ZectrixButtonEvent ev;
            if (!board_.WaitButton(&ev, pdMS_TO_TICKS(20000))) break;
            if (ev.button == ZectrixButton::kDown && ev.action == ZectrixButtonAction::kLongPress) {
                RequestPowerOrConfirm(PowerAction::kSleep);
                return;
            }
            if (ev.action != ZectrixButtonAction::kClick) continue;
            if (ev.button == ZectrixButton::kUp) {
                break;  // UP = immediate cancel
            } else if (ev.button == ZectrixButton::kDown) {
                choice = (choice + 1) % 3;
                ui_.RenderPowerMenuPopup(choice);
            } else if (ev.button == ZectrixButton::kOk) {
                if (choice == 0) {
                    RequestPowerOrConfirm(PowerAction::kReboot);
                    return;
                } else if (choice == 1) {
                    RequestPowerOrConfirm(PowerAction::kShutdown);
                    return;
                }
                break;  // choice == 2: cancel
            }
        }
        board_.DrainButtons();
        RenderCurrentTab(false);
    }

    // ========================================================================
    // Pure Single-Hop Xiaozhi Opus + WebSocket + Device Fridge MCP PTT Flow:
    //   Hold OK -> Check OTA & open WS in parallel while recording 16kHz PCM ->
    //   Encode 60ms Opus frames -> stream to xiaozhi.me or LAN NAS ->
    //   Device MCP executes fridge actions directly -> Big center confirmation!
    // ========================================================================
    void RunPttVoiceFlow() {
        const AppConfig cfg = store_.GetConfig();
        if (!wifi_web_.is_wifi_connected()) {
            audio_.PlayChimeError();
            ShowBriefPopup(false, "", "当前未连 Wi-Fi：请回到顶部菜单进入[设置]连上家庭 2.4G Wi-Fi", false);
            RenderCurrentTab(false);
            return;
        }

        // A notice may still be connecting/speaking: cancel it so the mic and service are free
        xiaozhi_.CancelAnnounce();
        for (int i = 0; i < 40 && xiaozhi_.is_announcing(); ++i) vTaskDelay(pdMS_TO_TICKS(50));
        audio_.TtsStop();

        XiaozhiOtaInfo ota;
        const bool ptt_begun = RunWithTimerTicking("ptt_open", 16384, false, [&]() {
            return xiaozhi_.BeginPttSession(cfg, &ota);
        });
        if (!ptt_begun) {
            if (ota.needs_activation) {
                audio_.PlayChimeStart();
                RunActivationPopup(cfg, ota);
                return;
            }
            audio_.PlayChimeError();
            ShowBriefPopup(false, "", ota.error.empty() ? "无法连接小智语音服务" : ota.error, false);
            RenderCurrentTab(false);
            return;
        }

        board_.SetPowerLed(true);
        audio_.PlayChimeStart();

        std::vector<uint8_t> wav;
        int recorded_ms = 0;
        const bool ok = audio_.RecordPttWav(
            &wav, 10,
            [this]() { return board_.IsButtonPressed(ZectrixButton::kOk); },
            &recorded_ms);
        audio_.PlayChimeStop();
        board_.DrainButtons();

        if (!ok || wav.empty()) {
            xiaozhi_.CancelPttSession();
            board_.SetPowerLed(false);
            audio_.PlayChimeError();
            ShowBriefPopup(false, "", "按住时间太短，请听到“叮”后再说话，说完松手", false);
            RenderCurrentTab(false);
            return;
        }

        // Opus encoding uses alloca heavily: give the worker the same 32KB stack as the main task
        // Show the result as soon as Xiaozhi has acted, while its spoken reply keeps playing
        std::string early_heard, early_text;
        bool early_undo = false;
        bool early_shown = false;
        const XiaozhiVoiceResult r = RunWithTimerTicking(
            "ptt_fin", 32768, true,
            [&]() { return xiaozhi_.FinishPttWithWav(wav, focus_ingredient_); },
            [&]() {
                if (early_shown) return;
                bool changed = false;
                if (!xiaozhi_.PeekTurnPreview(&early_heard, &early_text, &changed)) return;
                early_undo = changed && store_.HasUndoSnapshot();
                ui_.RenderVoiceResultPopup(true, early_heard.empty() ? "语音指令" : early_heard,
                                           early_text, early_undo);
                early_shown = true;
            });
        wav.clear();
        wav.shrink_to_fit();
        board_.SetPowerLed(false);

        if (!r.ok) {
            audio_.PlayChimeError();
            store_.RecordAiLog(r.heard.empty() ? "[小智语音]" : r.heard,
                               r.error, r.latency_ms, false);
            ShowBriefPopup(false, "", r.error, false);
            RenderCurrentTab(false);
            return;
        }

        store_.RecordAiLog(r.heard.empty() ? "[小智语音]" : r.heard,
                           r.reply, r.latency_ms, true);
        audio_.PlayChimeOk();

        if (r.step_delta != 0) {
            const auto recipes = store_.GetMatchedRecipesSorted(focus_ingredient_);
            int max_step = 0;
            if (!recipes.empty()) {
                const int ri = std::clamp(recipe_idx_, 0, static_cast<int>(recipes.size()) - 1);
                max_step = std::max(0, static_cast<int>(recipes[ri].recipe.steps.size()) - 1);
            }
            cook_step_idx_ = std::clamp(cook_step_idx_ + r.step_delta, 0, max_step);
            tab_ = FridgeTab::kCook;
            nav_focus_ = false;
        } else if (r.recipe_updated) {
            focus_ingredient_.clear();
            tab_ = FridgeTab::kRecipe;
            nav_focus_ = false;
            recipe_idx_ = 0;
            cook_step_idx_ = 0;
            selected_ = 0;
        } else if (r.timer_started) {
            tab_ = FridgeTab::kCook;
            nav_focus_ = false;
        }

        const std::string final_heard = r.heard.empty() ? "语音指令" : r.heard;
        const bool final_undo = r.data_changed && store_.HasUndoSnapshot();
        const bool same_as_early = early_shown && early_text == r.reply && early_undo == final_undo &&
                                   (early_heard.empty() ? "语音指令" : early_heard) == final_heard;
        const bool want_undo = ShowBriefPopup(true, final_heard, r.reply, final_undo,
                                              /*already_rendered=*/same_as_early);
        if (want_undo && store_.UndoLastVoiceAction()) {
            audio_.PlayChimeStop();
        }
        RenderCurrentTab(false);
    }

    // ========================================================================
    // Hardware-hardened Deep Sleep Standby (Page 0 4bpp 16-gray poster!)
    // Pin-level guarded so plugged-in USB NEVER causes instant wake-loop!
    // ========================================================================
    [[noreturn]] void EnterDeepSleepStandby(bool force_tomorrow_alarm = false) {
        ESP_LOGI(kTag, "entering hardware-verified deep sleep standby");
        const AppConfig cfg = store_.GetConfig();
        const ZectrixPowerSnapshot pwr = board_.ReadPowerSnapshot();
        const auto all_items = store_.GetItemsFiltered(ZoneFilter::kAll);
        const auto matched = store_.GetMatchedRecipesSorted();
        const RecipeMatchView* top_rec = matched.empty() ? nullptr : &matched[0];

        SyncNfcShopUrlIfNeeded(NowMs(), false);
        ui_.RenderStandby4bpp(TodayChineseDateWeekday(), FridgeStore::NowHmString(),
                              pwr.battery_valid ? pwr.battery_percent : 0, pwr.charge.charging,
                              all_items, top_rec, store_.GetShoppingList(), cfg.wake_hour,
                              cfg.standby_style);

        if (board_.nfc() != nullptr) {
            board_.nfc()->PowerOff();
        }

        if (board_.rtc() != nullptr) {
            struct tm alarm_tm = {};
            const time_t now = std::time(nullptr);
            localtime_r(&now, &alarm_tm);
            if (alarm_tm.tm_year + 1900 >= 2024) {
                if (force_tomorrow_alarm || alarm_tm.tm_hour >= cfg.wake_hour) {
                    const time_t tomorrow = now + 86400;
                    localtime_r(&tomorrow, &alarm_tm);
                }
                alarm_tm.tm_hour = cfg.wake_hour;
                alarm_tm.tm_min = 0;
                alarm_tm.tm_sec = 0;
                board_.rtc()->ClearAlarmFlag();
                board_.rtc()->SetAlarm(alarm_tm);
                board_.rtc()->EnableInterrupt(true);
            } else {
                board_.rtc()->DisableAlarm();
                board_.rtc()->ClearAlarmFlag();
            }
        }

        wifi_web_.Stop();
        board_.SetPowerLed(false);
        board_.SetAudioPower(false);

        // Wait until buttons are released so EXT1 wakeup doesn't re-trigger immediately
        for (int i = 0; i < 40 && (board_.IsButtonPressed(ZectrixButton::kDown) ||
                                   board_.IsButtonPressed(ZectrixButton::kOk));
             ++i) {
            vTaskDelay(pdMS_TO_TICKS(50));
        }

        // Keep battery power latch (GPIO17) locked HIGH across Deep Sleep!
        gpio_hold_en(ZECTRIX_VBAT_LATCH);
        gpio_deep_sleep_hold_en();

        // Lock RTC-domain pull-ups on active-low wake pins so sleep_gpio isolation NEVER floats them LOW!
        rtc_gpio_pullup_en(ZECTRIX_BUTTON_OK);
        rtc_gpio_pulldown_dis(ZECTRIX_BUTTON_OK);
        rtc_gpio_pullup_en(ZECTRIX_BUTTON_DOWN);
        rtc_gpio_pulldown_dis(ZECTRIX_BUTTON_DOWN);
        rtc_gpio_pullup_en(ZECTRIX_RTC_INT);
        rtc_gpio_pulldown_dis(ZECTRIX_RTC_INT);
        vTaskDelay(pdMS_TO_TICKS(20));

        const bool ok_high = gpio_get_level(ZECTRIX_BUTTON_OK) != 0;
        const bool down_high = gpio_get_level(ZECTRIX_BUTTON_DOWN) != 0;
        const bool rtc_high = gpio_get_level(ZECTRIX_RTC_INT) != 0;
        const uint64_t wake_mask = ComputeSafeExt1WakeMask(ok_high, down_high, rtc_high);
        if (wake_mask != 0) {
            ESP_LOGI(kTag, "EXT1 wake_mask=0x%llx (ANY_LOW, ok=%d down=%d rtc=%d)",
                     static_cast<unsigned long long>(wake_mask),
                     ok_high, down_high, rtc_high);
            esp_sleep_enable_ext1_wakeup(wake_mask, ESP_EXT1_WAKEUP_ANY_LOW);
        } else {
            // All 3 active-low pins are stuck LOW (e.g. jammed button / fridge magnet):
            // NEVER fall back to ANY_LOW on OK (would instant-wake loop!).
            // Arm ANY_HIGH on OK|DOWN so device sleeps quietly and wakes once released.
            const uint64_t release_mask =
                (1ULL << ZECTRIX_BUTTON_OK) | (1ULL << ZECTRIX_BUTTON_DOWN);
            ESP_LOGW(kTag, "All wake pins stuck LOW; arming ANY_HIGH release wake_mask=0x%llx",
                     static_cast<unsigned long long>(release_mask));
            esp_sleep_enable_ext1_wakeup(release_mask, ESP_EXT1_WAKEUP_ANY_HIGH);
        }
        esp_deep_sleep_start();
    }

    [[noreturn]] void RebootDevice() {
        ESP_LOGI(kTag, "restarting device cleanly");
        xiaozhi_.CancelAnnounce();
        audio_.TtsStop();
        ui_.ShowBootSplash("正在重启极趣小冰箱...");
        wifi_web_.Stop();
        board_.SetPowerLed(false);
        board_.SetAudioPower(false);
        vTaskDelay(pdMS_TO_TICKS(150));
        esp_restart();
    }

    [[noreturn]] void FullClearShutdown() {
        ESP_LOGI(kTag, "clearing display & cutting battery power latch");
        xiaozhi_.CancelAnnounce();
        audio_.TtsStop();
        ui_.ClearDisplay();
        if (board_.nfc() != nullptr) board_.nfc()->PowerOff();
        if (board_.rtc() != nullptr) {
            board_.rtc()->DisableAlarm();
            board_.rtc()->ClearAlarmFlag();
        }
        wifi_web_.Stop();
        board_.SetPowerLed(false);
        board_.SetAudioPower(false);
        for (int i = 0; i < 30 && board_.IsButtonPressed(ZectrixButton::kOk); ++i) {
            vTaskDelay(pdMS_TO_TICKS(50));
        }
        vTaskDelay(pdMS_TO_TICKS(80));
        board_.CutBatteryPower();
        vTaskDelay(pdMS_TO_TICKS(100));
        // If USB power stays connected after cutting VBAT_LATCH, let OK wake the device back up.
        rtc_gpio_pullup_en(ZECTRIX_BUTTON_OK);
        rtc_gpio_pulldown_dis(ZECTRIX_BUTTON_OK);
        vTaskDelay(pdMS_TO_TICKS(10));
        if (gpio_get_level(ZECTRIX_BUTTON_OK) != 0) {
            esp_sleep_enable_ext1_wakeup(1ULL << ZECTRIX_BUTTON_OK, ESP_EXT1_WAKEUP_ANY_LOW);
        }
        esp_deep_sleep_start();
    }

    ZectrixBoard board_;
    zectrix_epd_handle_t epd_ = nullptr;
    FridgeStore store_;
    AudioService audio_;
    XiaozhiClient xiaozhi_;
    WifiWebService wifi_web_;
    FridgeUi ui_;

    FridgeTab tab_ = FridgeTab::kFridge;
    int selected_ = 0;
    bool nav_focus_ = true;  // true = top menu bar has focus
    bool reminder_check_pending_ = false;
    int32_t reminder_attempts_day_ = 0;
    int reminder_attempts_ = 0;
    int recipe_idx_ = 0;
    int cook_step_idx_ = 0;
    std::string focus_ingredient_;

    bool alarm_shown_ = false;
    bool last_net_wifi_ = false;
    bool last_net_ap_ = false;
    int64_t last_activity_ms_ = 0;
    int64_t last_nfc_sync_attempt_ms_ = 0;
    std::string last_nfc_url_;
    std::atomic<bool> ui_dirty_{false};
    std::atomic<int> pending_tab_{-1};
};

}  // namespace

extern "C" void app_main(void) {
    static FridgeApp app;
    app.Run();
}
