#ifndef ZECTRIX_FRIDGE_WIFI_WEB_SERVICE_H_
#define ZECTRIX_FRIDGE_WIFI_WEB_SERVICE_H_

#include <atomic>
#include <functional>
#include <mutex>
#include <string>

#include "audio_service.h"
#include "esp_http_server.h"
#include "fridge_store.h"
#include "xiaozhi_client.h"
#include "zectrix_board.h"

class WifiWebService {
public:
    // notify(switch_to_recipe, switch_to_cook)
    using NotifyCallback = std::function<void(bool, bool)>;

    WifiWebService(ZectrixBoard* board, FridgeStore* store,
                   AudioService* audio = nullptr, XiaozhiClient* xiaozhi = nullptr);
    ~WifiWebService();

    void Start(NotifyCallback on_ui_dirty);
    void Stop();
    void Poll();
    void ReconnectStaOrAp(bool force_ap = false, bool manual_request = false);
    bool TryAutoRecoverSta();
    void SyncRtcFromSystem();
    void LoadSystemTimeFromRtc();

    bool is_wifi_connected() const { return wifi_connected_.load(); }
    bool is_ap_mode() const { return ap_mode_.load(); }
    std::string ip_address() const;
    std::string access_url() const;
    std::string ap_wifi_qr_payload() const;
    const std::string& ap_password() const { return ap_pass_; }

private:
    static void WifiEventHandler(void* arg, esp_event_base_t base, int32_t id, void* data);
    void HandleWifiEvent(esp_event_base_t base, int32_t id, void* data);
    void StartHttpServer();
    void StopHttpServer();
    void StartSntpIfNeeded();
    void EnsureApPassword();

    static esp_err_t HandleIndex(httpd_req_t* req);
    static esp_err_t HandleGetState(httpd_req_t* req);
    static esp_err_t HandleItems(httpd_req_t* req);
    static esp_err_t HandleShopping(httpd_req_t* req);
    static esp_err_t HandleTimers(httpd_req_t* req);
    static esp_err_t HandleConfig(httpd_req_t* req);
    static esp_err_t HandleAiText(httpd_req_t* req);
    static esp_err_t HandleAiTest(httpd_req_t* req);
    static esp_err_t HandleAiStatus(httpd_req_t* req);
    static esp_err_t HandleRefresh(httpd_req_t* req);
    static esp_err_t HandleDiagMic(httpd_req_t* req);

    struct AiJobState {
        uint32_t id = 0;
        bool running = false;
        bool done = false;
        bool ok = false;
        bool is_test = false;
        bool needs_activation = false;
        int ms = 0;
        std::string reply;
        std::string error;
        std::string ws_url;
        std::string mac;
        std::string activation_code;
    };

    static void AiWorkerTask(void* arg);
    void RunAiTextWorker(uint32_t job_id, AppConfig cfg, std::string text);
    void RunAiTestWorker(uint32_t job_id, AppConfig cfg);

    static bool ReadJsonBody(httpd_req_t* req, std::string* out);
    static esp_err_t SendJson(httpd_req_t* req, const std::string& json);
    void NotifyUi(bool recipe = false, bool cook = false);

    ZectrixBoard* board_ = nullptr;
    FridgeStore* store_ = nullptr;
    AudioService* audio_ = nullptr;
    XiaozhiClient* xiaozhi_ = nullptr;
    NotifyCallback on_ui_dirty_;
    httpd_handle_t server_ = nullptr;
    bool wifi_inited_ = false;
    bool sntp_started_ = false;
    std::mutex wifi_mutex_;
    std::atomic<int> sta_retry_count_{0};
    std::atomic<bool> pending_ap_fallback_{false};
    std::atomic<bool> pending_sta_reconnect_{false};

    std::atomic<bool> wifi_connected_{false};
    std::atomic<bool> ap_mode_{false};
    std::atomic<bool> manual_ap_{false};
    mutable std::mutex ip_mutex_;
    std::string ip_str_ = "192.168.4.1";

    std::mutex ai_job_mutex_;
    AiJobState ai_job_;
    std::string ap_pass_;
};

#endif  // ZECTRIX_FRIDGE_WIFI_WEB_SERVICE_H_
