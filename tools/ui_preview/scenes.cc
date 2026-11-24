// Renders every e-paper page & popup with realistic sample data -> out/*.pgm
#include <cstdio>
#include <string>
#include <vector>

#include "fridge_store.h"
#include "fridge_ui.h"

bool DumpPanelPgm(const std::string& path);

namespace {
std::string g_out = "out";
int g_fail = 0;
void Shot(const char* name) {
    if (!DumpPanelPgm(g_out + "/" + name + ".pgm")) { std::fprintf(stderr, "dump failed: %s\n", name); g_fail++; }
}
void Check(esp_err_t e, const char* what) {
    if (e != ESP_OK) { std::fprintf(stderr, "render failed: %s (%d)\n", what, e); g_fail++; }
}
}  // namespace

int main(int argc, char** argv) {
    if (argc > 1) g_out = argv[1];
    static FridgeStore store;
    store.Init();
    store.AddOrUpdateItem("酸奶", "4杯", "冷藏", 1);
    store.AddOrUpdateItem("嫩豆腐", "1盒", "冷藏", 0);
    store.AddOrUpdateItem("西兰花", "1颗", "冷藏", 3);
    store.AddOrUpdateItem("五花肉", "500g", "冷冻", 30);
    store.AddOrUpdateItem("剩米饭", "1碗", "冷藏", -1);
    store.AddOrUpdateItem("草莓", "1盒", "冷藏", 2);
    store.AddOrUpdateItem("橙汁", "1L", "冷藏", 6);
    store.AddOrUpdateItem("速冻水饺", "1袋", "冷冻", 60);
    store.StartTimer("炖排骨", 25, 0);

    auto* epd = reinterpret_cast<zectrix_epd_handle_t>(0x1);
    static FridgeUi ui(epd);

    UiTopBar bar;
    bar.hm_time = "18:42";
    bar.wifi_ok = true;
    bar.battery_pct = 76;
    UiTopBar bar_timer = bar;
    bar_timer.active_timer_label = "剩24分";

    int all = 0, ex = 0, fr = 0, fz = 0;
    const auto items = store.GetItemsFiltered(ZoneFilter::kAll, &all, &ex, &fr, &fz);
    const auto recipes = store.GetMatchedRecipesSorted();
    const auto shopping = store.GetShoppingList();
    const auto timers = store.GetTimers();

    Check(ui.ShowBootSplash("正在连接 Wi-Fi..."), "boot"); Shot("00_boot");
    Check(ui.RenderStandby4bpp("11/24 周二", "18:42", 76, false, items,
                               recipes.empty() ? nullptr : &recipes[0], shopping, 8), "standby");
    Shot("01_standby_4bpp");

    UiTopBar bar_menu = bar;
    bar_menu.nav_focus = true;
    Check(ui.RenderFridgePage(bar_menu, all, fr, fz, items, -1, true), "fridge menu"); Shot("02a_fridge_menu_focus");
    Check(ui.RenderRecipePage(bar_menu, recipes, 0, -1, true), "recipe menu"); Shot("02b_recipe_menu_focus");
    Check(ui.RenderFridgePage(bar, all, fr, fz, items, 1, true), "fridge"); Shot("02_fridge");
    Check(ui.RenderFridgePage(bar_timer, all, fr, fz, items, 9, true), "fridge p2"); Shot("03_fridge_page2_timer");
    Check(ui.RenderItemPopup(items[0], 0, true), "item popup"); Shot("04_item_popup");
    Check(ui.RenderFridgePage(bar, 0, 0, 0, {}, 0, true), "fridge empty"); Shot("05_fridge_empty");

    Check(ui.RenderRecipePage(bar, recipes, 0, 0, true), "recipe"); Shot("06_recipe");
    Check(ui.RenderRecipePage(bar, recipes, 1, 1, true), "recipe2"); Shot("07_recipe_btn2");

    const RecipeMatchView* r0 = recipes.empty() ? nullptr : &recipes[0];
    Check(ui.RenderCookPage(bar_timer, r0, 1, timers, true), "cook"); Shot("08_cook");
    if (r0) {
        std::vector<KitchenTimer> step_timers = timers;
        step_timers.push_back({999, r0->recipe.title, 120, 0, true, false});
        Check(ui.RenderCookPage(bar_timer, r0, 1, step_timers, true), "cook running"); Shot("08b_cook_timer_running");
    }
    if (r0) {
        Check(ui.RenderCookPage(bar, r0, static_cast<int>(r0->recipe.steps.size()) - 1, {}, true), "cook last");
        Shot("09_cook_last");
        Check(ui.RenderCookDonePopup(*r0, 0), "cook done"); Shot("10_cook_done_popup");
    }

    Check(ui.RenderShoppingPage(bar, shopping, 1, true), "shopping"); Shot("11_shopping");
    Check(ui.RenderShoppingPage(bar, shopping, static_cast<int>(shopping.size()), true), "shopping commit");
    Shot("12_shopping_commit");

    AppConfig cfg = store.GetConfig();
    cfg.wifi_ssid = "MyHome";  // never put real SSID into preview images
    cfg.ota_url = "https://api.tenclass.net/xiaozhi/ota/";
    ZectrixPowerSnapshot pwr;
    pwr.battery_valid = true; pwr.battery_percent = 76; pwr.charge.power_present = false;
    Check(ui.RenderSettingsPage(bar, cfg, "192.168.1.50", "http://192.168.1.50/", "手机扫码管理", "",
                                pwr, 0, true), "settings");
    Shot("13_settings");
    UiTopBar bar_ap = bar; bar_ap.ap_mode = true; bar_ap.wifi_ok = false;
    Check(ui.RenderSettingsPage(bar_ap, cfg, "192.168.4.1", "WIFI:T:WPA;S:ZecTrix-Fridge;P:preview;;",
                                "扫码连接热点", "demo2345", pwr, 1, true), "settings ap");
    Shot("14_settings_ap");

    Check(ui.RenderFridgePage(bar, all, fr, fz, items, 0, true), "base");
    Check(ui.RenderVoiceResultPopup(true, "放两盒牛奶五天过期", "已放入鲜牛奶 2盒 (冷藏 · 剩5天)\n已加采购单:小葱 1把", true), "voice ok");
    Shot("15_voice_ok");
    Check(ui.RenderFridgePage(bar, all, fr, fz, items, 0, true), "base");
    Check(ui.RenderVoiceResultPopup(false, "", "没听清，请靠近一点再说一次", false), "voice fail");
    Shot("16_voice_fail");
    Check(ui.RenderSettingsPage(bar, cfg, "192.168.1.50", "http://192.168.1.50/", "手机扫码管理", "", pwr, 0, true), "base");
    Check(ui.RenderVoiceHelpPopup(), "help"); Shot("17_voice_help");
    Check(ui.RenderSettingsPage(bar, cfg, "192.168.1.50", "http://192.168.1.50/", "手机扫码管理", "", pwr, 0, true), "base");
    Check(ui.RenderActivationPopup("428615", ""), "activation"); Shot("18_activation");
    Check(ui.RenderRecipePage(bar, recipes, 0, 0, true), "base");
    Check(ui.ShowQaPopup("小智回答", "西兰花焯水一分钟后过凉水，口感更脆，颜色也更绿。焯水时加一点盐和几滴油效果更好。"), "qa");
    Shot("19_qa");
    Check(ui.RenderFridgePage(bar, all, fr, fz, items, 0, true), "base");
    Check(ui.RenderLoadingPopup("正在定制食谱...", "正用“西兰花”搭配冰箱现有库存生成家常菜，请稍候..."), "loading");
    Shot("20_loading");
    Check(ui.RenderCookPage(bar_timer, r0, 1, timers, true), "base");
    Check(ui.RenderSleepTimerConfirmPopup("炖排骨 剩24分", 0), "sleep confirm"); Shot("21_sleep_confirm");
    std::vector<KitchenTimer> ringing = timers;
    if (!ringing.empty()) { ringing[0].active = false; ringing[0].ringing = true; }
    Check(ui.ShowAlarmScreen(ringing), "alarm"); Shot("22_alarm");

    std::fprintf(stderr, "%s\n", g_fail ? "SOME SCENES FAILED" : "all scenes rendered");
    return g_fail ? 1 : 0;
}
