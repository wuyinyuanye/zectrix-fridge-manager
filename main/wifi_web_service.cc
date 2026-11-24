#include "wifi_web_service.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <memory>
#include <sys/time.h>

#include "cJSON.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_sntp.h"
#include "esp_random.h"
#include "esp_wifi.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "rtc_pcf8563.h"
#include "web_ui_html.h"

namespace {

constexpr char kTag[] = "wifi_web";
constexpr char kApSsid[] = "ZecTrix-Fridge";
constexpr int kMaxStaRetries = 10;

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

}  // namespace

WifiWebService::WifiWebService(ZectrixBoard* board, FridgeStore* store,
                               AudioService* audio, XiaozhiClient* xiaozhi)
    : board_(board), store_(store), audio_(audio), xiaozhi_(xiaozhi) {}

WifiWebService::~WifiWebService() { Stop(); }

void WifiWebService::LoadSystemTimeFromRtc() {
    setenv("TZ", "CST-8", 1);
    tzset();
    if (!board_ || !board_->rtc()) return;
    struct tm rtc_tm = {};
    if (board_->rtc()->GetTime(rtc_tm) && rtc_tm.tm_year + 1900 >= 2025) {
        const time_t t = mktime(&rtc_tm);
        struct timeval tv = {.tv_sec = t, .tv_usec = 0};
        settimeofday(&tv, nullptr);
    }
}

void WifiWebService::SyncRtcFromSystem() {
    if (!board_ || !board_->rtc()) return;
    const time_t now = std::time(nullptr);
    struct tm lt = {};
    localtime_r(&now, &lt);
    if (lt.tm_year + 1900 >= 2025) {
        board_->rtc()->SetTime(lt);
    }
}

void WifiWebService::Start(NotifyCallback on_ui_dirty) {
    on_ui_dirty_ = std::move(on_ui_dirty);
    LoadSystemTimeFromRtc();

    if (!wifi_inited_) {
        ESP_ERROR_CHECK(esp_netif_init());
        esp_err_t ev_err = esp_event_loop_create_default();
        if (ev_err != ESP_OK && ev_err != ESP_ERR_INVALID_STATE) ESP_ERROR_CHECK(ev_err);
        esp_netif_create_default_wifi_sta();
        esp_netif_create_default_wifi_ap();
        wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
        ESP_ERROR_CHECK(esp_wifi_init(&cfg));
        ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &WifiEventHandler, this));
        ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &WifiEventHandler, this));
        wifi_inited_ = true;
    }

    EnsureApPassword();
    ReconnectStaOrAp(false);
    StartHttpServer();
}

// Per-device random WPA2 password for the setup hotspot, generated once and kept in NVS.
// Shown on the settings page and inside the AP QR code; never a shared hard-coded secret.
void WifiWebService::EnsureApPassword() {
    if (!ap_pass_.empty()) return;
    constexpr char kNs[] = "fridge_v2";
    constexpr char kKey[] = "ap_pass";
    nvs_handle_t h = 0;
    if (nvs_open(kNs, NVS_READWRITE, &h) == ESP_OK) {
        char buf[24] = {};
        size_t len = sizeof(buf);
        if (nvs_get_str(h, kKey, buf, &len) == ESP_OK && std::strlen(buf) >= 8) {
            ap_pass_ = buf;
        }
    }
    if (ap_pass_.empty()) {
        // No look-alike characters (0/o, 1/l/i) so it is easy to type from the screen
        static constexpr char kAlphabet[] = "abcdefghjkmnpqrstuvwxyz23456789";
        constexpr size_t kAlphabetLen = sizeof(kAlphabet) - 1;
        for (int i = 0; i < 8; ++i) {
            ap_pass_.push_back(kAlphabet[esp_random() % kAlphabetLen]);
        }
        if (h != 0) {
            if (nvs_set_str(h, kKey, ap_pass_.c_str()) != ESP_OK || nvs_commit(h) != ESP_OK) {
                ESP_LOGW(kTag, "could not persist AP password; a new one will be generated next boot");
            }
        }
    }
    if (h != 0) nvs_close(h);
}

void WifiWebService::Stop() {
    StopHttpServer();
    {
        std::lock_guard<std::mutex> lock(wifi_mutex_);
        pending_ap_fallback_.store(false);
        pending_sta_reconnect_.store(false);
        if (wifi_inited_) {
            esp_err_t err = esp_wifi_stop();
            if (err != ESP_OK && err != ESP_ERR_WIFI_NOT_INIT) {
                ESP_LOGW(kTag, "esp_wifi_stop: %s", esp_err_to_name(err));
            }
        }
        wifi_connected_.store(false);
        ap_mode_.store(false);
    }
    // Wait briefly for any in-flight web AI worker task to unwind cleanly after Wi-Fi stops
    for (int i = 0; i < 25; ++i) {
        bool running = false;
        {
            std::lock_guard<std::mutex> lk(ai_job_mutex_);
            running = ai_job_.running;
        }
        if (!running) break;
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

void WifiWebService::Poll() {
    if (pending_sta_reconnect_.exchange(false)) {
        pending_ap_fallback_.store(false);
        ReconnectStaOrAp(false, false);
    } else if (pending_ap_fallback_.exchange(false)) {
        ReconnectStaOrAp(true, false);
    }
}

void WifiWebService::ReconnectStaOrAp(bool force_ap, bool manual_request) {
    std::lock_guard<std::mutex> lock(wifi_mutex_);
    if (!wifi_inited_) return;
    pending_ap_fallback_.store(false);
    esp_err_t err = esp_wifi_stop();
    if (err != ESP_OK && err != ESP_ERR_WIFI_NOT_STARTED) {
        ESP_LOGW(kTag, "esp_wifi_stop: %s", esp_err_to_name(err));
    }
    wifi_connected_.store(false);
    sta_retry_count_.store(0);

    const AppConfig cfg = store_->GetConfig();
    if (force_ap || cfg.wifi_ssid.empty()) {
        ap_mode_.store(true);
        manual_ap_.store(manual_request || cfg.wifi_ssid.empty());
        { std::lock_guard<std::mutex> ip_lock(ip_mutex_); ip_str_ = "192.168.4.1"; }
        wifi_config_t ap = {};
        std::strncpy(reinterpret_cast<char*>(ap.ap.ssid), kApSsid, sizeof(ap.ap.ssid) - 1);
        std::strncpy(reinterpret_cast<char*>(ap.ap.password), ap_pass_.c_str(), sizeof(ap.ap.password) - 1);
        ap.ap.ssid_len = static_cast<uint8_t>(std::strlen(kApSsid));
        ap.ap.channel = 6;
        ap.ap.max_connection = 4;
        ap.ap.authmode = WIFI_AUTH_WPA2_PSK;
        if ((err = esp_wifi_set_mode(WIFI_MODE_AP)) != ESP_OK ||
            (err = esp_wifi_set_config(WIFI_IF_AP, &ap)) != ESP_OK ||
            (err = esp_wifi_start()) != ESP_OK) {
            ESP_LOGE(kTag, "Failed starting AP mode: %s", esp_err_to_name(err));
        }
        return;
    }

    ap_mode_.store(false);
    manual_ap_.store(false);
    wifi_config_t sta = {};
    std::strncpy(reinterpret_cast<char*>(sta.sta.ssid), cfg.wifi_ssid.c_str(), sizeof(sta.sta.ssid) - 1);
    std::strncpy(reinterpret_cast<char*>(sta.sta.password), cfg.wifi_pass.c_str(), sizeof(sta.sta.password) - 1);
    sta.sta.threshold.authmode = cfg.wifi_pass.empty() ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK;
    if ((err = esp_wifi_set_mode(WIFI_MODE_STA)) != ESP_OK ||
        (err = esp_wifi_set_config(WIFI_IF_STA, &sta)) != ESP_OK ||
        (err = esp_wifi_start()) != ESP_OK) {
        ESP_LOGE(kTag, "Failed starting STA mode: %s", esp_err_to_name(err));
    }
}

bool WifiWebService::TryAutoRecoverSta() {
    if (!wifi_inited_ || !ap_mode_.load() || manual_ap_.load()) return false;
    if (store_->GetConfig().wifi_ssid.empty()) return false;
    wifi_sta_list_t sta_list = {};
    if (esp_wifi_ap_get_sta_list(&sta_list) == ESP_OK && sta_list.num > 0) {
        return false;  // Phone is actively connected to AP portal — do not interrupt!
    }
    ESP_LOGI(kTag, "auto-recovering saved home Wi-Fi STA after router outage");
    ReconnectStaOrAp(false, false);
    return true;
}

void WifiWebService::WifiEventHandler(void* arg, esp_event_base_t base, int32_t id, void* data) {
    static_cast<WifiWebService*>(arg)->HandleWifiEvent(base, id, data);
}

void WifiWebService::HandleWifiEvent(esp_event_base_t base, int32_t id, void* data) {
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_connected_.store(false);
        if (!ap_mode_.load()) {
            if (sta_retry_count_.fetch_add(1) + 1 <= kMaxStaRetries) {
                esp_wifi_connect();
            } else {
                // Defer STA->AP switch to Poll() so we never call esp_wifi_stop() inside sys_evt!
                pending_ap_fallback_.store(true);
            }
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        auto* evt = static_cast<ip_event_got_ip_t*>(data);
        char buf[32];
        std::snprintf(buf, sizeof(buf), IPSTR, IP2STR(&evt->ip_info.ip));
        { std::lock_guard<std::mutex> lock(ip_mutex_); ip_str_ = buf; }
        sta_retry_count_.store(0);
        pending_ap_fallback_.store(false);
        wifi_connected_.store(true);
        ap_mode_.store(false);
        StartSntpIfNeeded();
    }
}

void WifiWebService::StartSntpIfNeeded() {
    if (sntp_started_) return;
    sntp_started_ = true;
    esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "ntp.aliyun.com");
    esp_sntp_setservername(1, "pool.ntp.org");
    esp_sntp_init();
}

std::string WifiWebService::ip_address() const {
    std::lock_guard<std::mutex> lock(ip_mutex_);
    return ip_str_;
}

std::string WifiWebService::access_url() const {
    return "http://" + ip_address() + "/";
}

std::string WifiWebService::ap_wifi_qr_payload() const {
    return std::string("WIFI:T:WPA;S:") + kApSsid + ";P:" + ap_pass_ + ";;";
}

void WifiWebService::NotifyUi(bool recipe, bool cook) {
    if (on_ui_dirty_) on_ui_dirty_(recipe, cook);
}

void WifiWebService::StartHttpServer() {
    if (server_) return;
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.stack_size = 16384;
    cfg.max_uri_handlers = 16;
    cfg.lru_purge_enable = true;
    if (httpd_start(&server_, &cfg) != ESP_OK) return;

    auto reg = [this](const char* uri, httpd_method_t m, esp_err_t (*h)(httpd_req_t*)) {
        httpd_uri_t u = {};
        u.uri = uri; u.method = m; u.handler = h; u.user_ctx = this;
        httpd_register_uri_handler(server_, &u);
    };
    reg("/", HTTP_GET, HandleIndex);
    reg("/api/state", HTTP_GET, HandleGetState);
    reg("/api/items", HTTP_POST, HandleItems);
    reg("/api/shopping", HTTP_POST, HandleShopping);
    reg("/api/timers", HTTP_POST, HandleTimers);
    reg("/api/recipes", HTTP_POST, HandleRecipes);
    reg("/api/config", HTTP_POST, HandleConfig);
    reg("/api/ai/text", HTTP_POST, HandleAiText);
    reg("/api/ai/test", HTTP_POST, HandleAiTest);
    reg("/api/ai/status", HTTP_GET, HandleAiStatus);
    reg("/api/refresh", HTTP_POST, HandleRefresh);
    reg("/api/diag/mic", HTTP_POST, HandleDiagMic);
}

void WifiWebService::StopHttpServer() {
    if (server_) { httpd_stop(server_); server_ = nullptr; }
}

bool WifiWebService::ReadJsonBody(httpd_req_t* req, std::string* out) {
    if (req->content_len <= 0 || req->content_len > 16384) {
        *out = "{}";
        return req->content_len == 0;
    }
    out->resize(static_cast<size_t>(req->content_len));
    size_t got = 0;
    while (got < out->size()) {
        const int r = httpd_req_recv(req, out->data() + got, out->size() - got);
        if (r <= 0) return false;
        got += static_cast<size_t>(r);
    }
    return true;
}

esp_err_t WifiWebService::SendJson(httpd_req_t* req, const std::string& json) {
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    return httpd_resp_send(req, json.c_str(), static_cast<ssize_t>(json.size()));
}

esp_err_t WifiWebService::HandleIndex(httpd_req_t* req) {
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, kWebUiHtml, HTTPD_RESP_USE_STRLEN);
}

esp_err_t WifiWebService::HandleGetState(httpd_req_t* req) {
    auto* self = static_cast<WifiWebService*>(req->user_ctx);
    const ZectrixPowerSnapshot pwr = self->board_->ReadPowerSnapshot();
    return SendJson(req, self->store_->ExportStateJson(
        self->ip_address(), self->is_wifi_connected(), self->is_ap_mode(),
        pwr.battery_valid ? pwr.battery_percent : 0, pwr.charge.charging));
}

esp_err_t WifiWebService::HandleItems(httpd_req_t* req) {
    auto* self = static_cast<WifiWebService*>(req->user_ctx);
    std::string body;
    if (!ReadJsonBody(req, &body)) return SendJson(req, "{\"ok\":false}");
    cJSON* root = cJSON_Parse(body.c_str());
    if (!root) return SendJson(req, "{\"ok\":false}");
    const std::string action = JStr(root, "action");
    const uint32_t id = static_cast<uint32_t>(JInt(root, "id", 0));
    if (action == "add") {
        const std::string name = JStr(root, "name");
        const std::string qty = JStr(root, "qty", "1份");
        const std::string zone = JStr(root, "zone");
        const int days = JInt(root, "days", -999);
        const std::string note = JStr(root, "note");
        cJSON_Delete(root);
        if (name.empty()) return SendJson(req, "{\"ok\":false,\"error\":\"请输入食材名称\"}");
        if (self->store_->AddOrUpdateItem(name, qty, zone, days, note) == 0) {
            return SendJson(req, "{\"ok\":false,\"error\":\"冰箱库存已满(最多80样)，请先清理\"}");
        }
        self->NotifyUi();
        return SendJson(req, "{\"ok\":true}");
    } else if (action == "del") {
        self->store_->RemoveItemById(id);
    } else if (action == "plus3") {
        self->store_->AdjustItemExpiry(id, 3);
    } else if (action == "shop") {
        cJSON_Delete(root);
        if (!self->store_->MoveItemToShopping(id)) {
            return SendJson(req, "{\"ok\":false,\"error\":\"采购清单已满(最多80样)或食材不存在\"}");
        }
        self->NotifyUi();
        return SendJson(req, "{\"ok\":true}");
    }
    cJSON_Delete(root);
    self->NotifyUi();
    return SendJson(req, "{\"ok\":true}");
}

esp_err_t WifiWebService::HandleShopping(httpd_req_t* req) {
    auto* self = static_cast<WifiWebService*>(req->user_ctx);
    std::string body;
    if (!ReadJsonBody(req, &body)) return SendJson(req, "{\"ok\":false}");
    cJSON* root = cJSON_Parse(body.c_str());
    if (!root) return SendJson(req, "{\"ok\":false}");
    const std::string action = JStr(root, "action");
    const uint32_t id = static_cast<uint32_t>(JInt(root, "id", 0));
    int moved = 0;
    int skipped_full = 0;
    if (action == "add") {
        const std::string name = JStr(root, "name");
        const std::string qty = JStr(root, "qty", "1份");
        cJSON_Delete(root);
        if (name.empty()) return SendJson(req, "{\"ok\":false,\"error\":\"请输入食材名称\"}");
        if (self->store_->AddShoppingItem(name, qty, "手机添加") == 0) {
            return SendJson(req, "{\"ok\":false,\"error\":\"采购清单已满(最多80样)，请先清理\"}");
        }
        self->NotifyUi();
        return SendJson(req, "{\"ok\":true,\"moved\":0,\"skipped_full\":0}");
    } else if (action == "toggle") {
        self->store_->ToggleShoppingItem(id);
    } else if (action == "del") {
        self->store_->RemoveShoppingItem(id);
    } else if (action == "commit") {
        moved = self->store_->CommitCheckedShoppingToFridge(&skipped_full);
    }
    cJSON_Delete(root);
    self->NotifyUi();
    return SendJson(req, "{\"ok\":true,\"moved\":" + std::to_string(moved) +
                             ",\"skipped_full\":" + std::to_string(skipped_full) + "}");
}

esp_err_t WifiWebService::HandleTimers(httpd_req_t* req) {
    auto* self = static_cast<WifiWebService*>(req->user_ctx);
    std::string body;
    if (!ReadJsonBody(req, &body)) return SendJson(req, "{\"ok\":false}");
    cJSON* root = cJSON_Parse(body.c_str());
    if (!root) return SendJson(req, "{\"ok\":false}");
    const std::string action = JStr(root, "action");
    bool started = false;
    if (action == "start") {
        started = self->store_->StartTimer(JStr(root, "label", "厨房计时"),
                                           JInt(root, "minutes", 10), JInt(root, "seconds", 0));
    } else if (action == "stop") {
        self->store_->StopTimer(static_cast<uint32_t>(JInt(root, "id", 0)));
    }
    cJSON_Delete(root);
    self->NotifyUi(false, started);
    return SendJson(req, "{\"ok\":true}");
}

esp_err_t WifiWebService::HandleRecipes(httpd_req_t* req) {
    auto* self = static_cast<WifiWebService*>(req->user_ctx);
    std::string body;
    if (!ReadJsonBody(req, &body)) return SendJson(req, "{\"ok\":false}");
    cJSON* root = cJSON_Parse(body.c_str());
    if (!root) return SendJson(req, "{\"ok\":false}");
    const std::string action = JStr(root, "action");
    const std::string title = JStr(root, "title");
    cJSON_Delete(root);
    if (action == "del") {
        if (title.empty() || !self->store_->RemoveRecipe(title)) {
            return SendJson(req, "{\"ok\":false,\"error\":\"未找到该菜谱\"}");
        }
        self->NotifyUi();
        return SendJson(req, "{\"ok\":true}");
    }
    return SendJson(req, "{\"ok\":false,\"error\":\"未知操作\"}");
}

esp_err_t WifiWebService::HandleConfig(httpd_req_t* req) {
    auto* self = static_cast<WifiWebService*>(req->user_ctx);
    std::string body;
    if (!ReadJsonBody(req, &body)) return SendJson(req, "{\"ok\":false}");
    cJSON* root = cJSON_Parse(body.c_str());
    if (!root) return SendJson(req, "{\"ok\":false}");

    AppConfig cfg = self->store_->GetConfig();
    const std::string new_ssid = JStr(root, "wifi_ssid", cfg.wifi_ssid.c_str());
    const std::string new_pass = JStr(root, "wifi_pass");
    const bool wifi_changed = (new_ssid != cfg.wifi_ssid) || !new_pass.empty();
    cfg.wifi_ssid = new_ssid;
    if (!new_pass.empty()) cfg.wifi_pass = new_pass;
    cfg.sleep_minutes = std::clamp(JInt(root, "sleep_minutes", cfg.sleep_minutes), 0, 720);
    cfg.wake_hour = std::clamp(JInt(root, "wake_hour", cfg.wake_hour), 0, 23);
    const std::string new_ota = JStr(root, "ota_url", cfg.ota_url.c_str());
    if (!new_ota.empty()) cfg.ota_url = new_ota;
    if (cJSON_HasObjectItem(root, "ws_token")) {
        const std::string tok = JStr(root, "ws_token");
        if (!tok.empty() || JInt(root, "clear_token", 0) != 0) cfg.ws_token = tok;
    }
    cJSON_Delete(root);

    self->store_->SaveConfig(cfg);
    if (self->xiaozhi_) self->xiaozhi_->InvalidateOtaCache();
    self->NotifyUi();

    esp_err_t ret = SendJson(req, std::string("{\"ok\":true,\"wifi_changed\":") +
                                      (wifi_changed ? "true}" : "false}"));
    if (wifi_changed) {
        self->pending_sta_reconnect_.store(true);
    }
    return ret;
}

namespace {
struct WebAiTaskArgs {
    WifiWebService* self = nullptr;
    uint32_t job_id = 0;
    bool is_test = false;
    AppConfig cfg;
    std::string text;
};
}  // namespace

void WifiWebService::AiWorkerTask(void* arg) {
    std::unique_ptr<WebAiTaskArgs> task(static_cast<WebAiTaskArgs*>(arg));
    if (task && task->self) {
        if (task->is_test) {
            task->self->RunAiTestWorker(task->job_id, std::move(task->cfg));
        } else {
            task->self->RunAiTextWorker(task->job_id, std::move(task->cfg), std::move(task->text));
        }
    }
    vTaskDelete(nullptr);
}

void WifiWebService::RunAiTextWorker(uint32_t job_id, AppConfig cfg, std::string text) {
    const XiaozhiVoiceResult r = xiaozhi_->SendTextCommand(cfg, text);
    if (!r.ok) {
        store_->RecordAiLog(text, r.error, r.latency_ms, false);
    } else {
        store_->RecordAiLog(text, r.reply, r.latency_ms, true);
        NotifyUi(r.recipe_updated, r.timer_started);
    }
    std::lock_guard<std::mutex> lock(ai_job_mutex_);
    if (ai_job_.id == job_id) {
        ai_job_.running = false;
        ai_job_.done = true;
        ai_job_.ok = r.ok;
        ai_job_.ms = r.latency_ms;
        ai_job_.reply = r.reply;
        ai_job_.error = r.error;
        ai_job_.needs_activation = r.needs_activation;
        ai_job_.activation_code = r.activation_code;
    }
}

void WifiWebService::RunAiTestWorker(uint32_t job_id, AppConfig cfg) {
    XiaozhiOtaInfo ota;
    int ms = 0;
    std::string err;
    const bool ok = xiaozhi_->TestConnection(cfg, &ota, &ms, &err);
    store_->RecordAiLog("[小智连通测试]", ok ? ("已连通 " + ota.ws_url) : err, ms, ok);
    std::lock_guard<std::mutex> lock(ai_job_mutex_);
    if (ai_job_.id == job_id) {
        ai_job_.running = false;
        ai_job_.done = true;
        ai_job_.ok = ok;
        ai_job_.ms = ms;
        ai_job_.ws_url = ota.ws_url;
        ai_job_.mac = XiaozhiClient::GetDeviceMac();
        ai_job_.needs_activation = ota.needs_activation;
        ai_job_.activation_code = ota.activation_code;
        ai_job_.error = err;
    }
}

esp_err_t WifiWebService::HandleAiText(httpd_req_t* req) {
    auto* self = static_cast<WifiWebService*>(req->user_ctx);
    std::string body;
    if (!ReadJsonBody(req, &body)) return SendJson(req, "{\"ok\":false,\"error\":\"请求无效\"}");
    cJSON* root = cJSON_Parse(body.c_str());
    if (!root) return SendJson(req, "{\"ok\":false,\"error\":\"JSON无效\"}");
    const std::string text = JStr(root, "text");
    cJSON_Delete(root);

    // 0ms local fast-path executes immediately without spawning a worker!
    const LocalFastAction fast = self->store_->TryLocalFastCommand(text);
    if (fast.handled) {
        self->store_->RecordAiLog(text, "[本地快通道] " + fast.receipt, 0, true);
        self->NotifyUi(false, fast.switch_to_cook);
        cJSON* o = cJSON_CreateObject();
        cJSON_AddBoolToObject(o, "ok", true);
        cJSON_AddNumberToObject(o, "ms", 0);
        cJSON_AddStringToObject(o, "reply", fast.receipt.c_str());
        char* s = cJSON_PrintUnformatted(o); cJSON_Delete(o);
        std::string out = s ? s : "{}"; if (s) cJSON_free(s);
        return SendJson(req, out);
    }

    if (!self->is_wifi_connected() || !self->xiaozhi_) {
        return SendJson(req, "{\"ok\":false,\"error\":\"设备尚未连接Wi-Fi\"}");
    }
    if (self->xiaozhi_->is_ptt_active()) {
        return SendJson(req, "{\"ok\":false,\"error\":\"语音服务正忙，请稍候再试\"}");
    }

    uint32_t job_id = 0;
    {
        std::lock_guard<std::mutex> lock(self->ai_job_mutex_);
        if (self->ai_job_.running) {
            return SendJson(req, "{\"ok\":false,\"error\":\"语音服务正忙，请稍候再试\"}");
        }
        job_id = ++self->ai_job_.id;
        self->ai_job_ = AiJobState{};
        self->ai_job_.id = job_id;
        self->ai_job_.running = true;
        self->ai_job_.is_test = false;
    }

    auto* args = new (std::nothrow) WebAiTaskArgs{self, job_id, false, self->store_->GetConfig(), text};
    if (!args || xTaskCreate(&WifiWebService::AiWorkerTask, "web_ai", 16384, args, 4, nullptr) != pdPASS) {
        delete args;
        std::lock_guard<std::mutex> lock(self->ai_job_mutex_);
        if (self->ai_job_.id == job_id) self->ai_job_.running = false;
        return SendJson(req, "{\"ok\":false,\"error\":\"后台任务启动失败，请重试\"}");
    }
    return SendJson(req, "{\"ok\":true,\"pending\":true,\"job_id\":" + std::to_string(job_id) + "}");
}

esp_err_t WifiWebService::HandleAiTest(httpd_req_t* req) {
    auto* self = static_cast<WifiWebService*>(req->user_ctx);
    if (!self->is_wifi_connected() || !self->xiaozhi_) {
        return SendJson(req, "{\"ok\":false,\"error\":\"请先连上家庭Wi-Fi\"}");
    }
    if (self->xiaozhi_->is_ptt_active()) {
        return SendJson(req, "{\"ok\":false,\"error\":\"语音服务正忙，请稍候再试\"}");
    }

    uint32_t job_id = 0;
    {
        std::lock_guard<std::mutex> lock(self->ai_job_mutex_);
        if (self->ai_job_.running) {
            return SendJson(req, "{\"ok\":false,\"error\":\"语音服务正忙，请稍候再试\"}");
        }
        job_id = ++self->ai_job_.id;
        self->ai_job_ = AiJobState{};
        self->ai_job_.id = job_id;
        self->ai_job_.running = true;
        self->ai_job_.is_test = true;
    }

    auto* args = new (std::nothrow) WebAiTaskArgs{self, job_id, true, self->store_->GetConfig(), {}};
    if (!args || xTaskCreate(&WifiWebService::AiWorkerTask, "web_ai", 16384, args, 4, nullptr) != pdPASS) {
        delete args;
        std::lock_guard<std::mutex> lock(self->ai_job_mutex_);
        if (self->ai_job_.id == job_id) self->ai_job_.running = false;
        return SendJson(req, "{\"ok\":false,\"error\":\"后台任务启动失败，请重试\"}");
    }
    return SendJson(req, "{\"ok\":true,\"pending\":true,\"job_id\":" + std::to_string(job_id) + "}");
}

esp_err_t WifiWebService::HandleAiStatus(httpd_req_t* req) {
    auto* self = static_cast<WifiWebService*>(req->user_ctx);
    AiJobState st;
    {
        std::lock_guard<std::mutex> lock(self->ai_job_mutex_);
        st = self->ai_job_;
    }
    cJSON* o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "job_id", st.id);
    cJSON_AddBoolToObject(o, "running", st.running);
    cJSON_AddBoolToObject(o, "done", st.done);
    cJSON_AddBoolToObject(o, "ok", st.ok);
    cJSON_AddBoolToObject(o, "is_test", st.is_test);
    cJSON_AddNumberToObject(o, "ms", st.ms);
    cJSON_AddStringToObject(o, "reply", st.reply.c_str());
    cJSON_AddStringToObject(o, "error", st.error.c_str());
    cJSON_AddStringToObject(o, "ws_url", st.ws_url.c_str());
    cJSON_AddStringToObject(o, "mac", st.mac.c_str());
    cJSON_AddBoolToObject(o, "needs_activation", st.needs_activation);
    cJSON_AddStringToObject(o, "activation_code", st.activation_code.c_str());
    char* s = cJSON_PrintUnformatted(o); cJSON_Delete(o);
    std::string out = s ? s : "{}"; if (s) cJSON_free(s);
    return SendJson(req, out);
}

esp_err_t WifiWebService::HandleRefresh(httpd_req_t* req) {
    auto* self = static_cast<WifiWebService*>(req->user_ctx);
    self->NotifyUi();
    return SendJson(req, "{\"ok\":true}");
}

esp_err_t WifiWebService::HandleDiagMic(httpd_req_t* req) {
    auto* self = static_cast<WifiWebService*>(req->user_ctx);
    if (!self->audio_) return SendJson(req, "{\"ok\":false,\"error\":\"no audio\"}");
    if (self->xiaozhi_ && self->xiaozhi_->is_ptt_active()) {
        return SendJson(req, "{\"ok\":false,\"error\":\"设备正在按键录音中\"}");
    }
    std::vector<uint8_t> wav;
    int ms = 0;
    bool busy = false;
    const bool ok = self->audio_->TryDiagnosticCapture(&wav, &ms, &busy);
    if (busy) {
        return SendJson(req, "{\"ok\":false,\"error\":\"声卡正忙，请稍候再试\"}");
    }
    cJSON* o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "ok", ok);
    cJSON_AddNumberToObject(o, "recorded_ms", ms);
    cJSON_AddNumberToObject(o, "bytes", static_cast<int>(wav.size()));
    if (ok && wav.size() > 44) {
        const size_t n = (wav.size() - 44) / 2;
        const auto* p = reinterpret_cast<const int16_t*>(wav.data() + 44);
        int64_t sum = 0;
        for (size_t i = 0; i < n; ++i) sum += p[i];
        const int32_t dc = static_cast<int32_t>(sum / static_cast<int64_t>(n));
        int32_t peak = 0;
        int64_t sq = 0;
        for (size_t i = 0; i < n; ++i) {
            const int32_t c = static_cast<int32_t>(p[i]) - dc;
            const int32_t a = c < 0 ? -c : c;
            if (a > peak) peak = a;
            sq += static_cast<int64_t>(c) * c;
        }
        cJSON_AddNumberToObject(o, "dc", dc);
        cJSON_AddNumberToObject(o, "peak", peak);
        cJSON_AddNumberToObject(o, "rms", static_cast<int>(std::sqrt(static_cast<double>(sq) / n)));
    }
    char* s = cJSON_PrintUnformatted(o); cJSON_Delete(o);
    std::string out = s ? s : "{}"; if (s) cJSON_free(s);
    return SendJson(req, out);
}
