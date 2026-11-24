#include "xiaozhi_client.h"

#include <arpa/inet.h>
#include <opus_encoder.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <memory>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_timer.h"

namespace {

constexpr char kTag[] = "xiaozhi";
constexpr size_t kMaxHttpResp = 16 * 1024;
constexpr int kOpusFrameMs = 60;
constexpr int kOpusFrameSamples = 16000 * kOpusFrameMs / 1000;  // 960 samples

constexpr EventBits_t kBitWsConnected = BIT0;
constexpr EventBits_t kBitHelloOk     = BIT1;
constexpr EventBits_t kBitMcpReady    = BIT2;
constexpr EventBits_t kBitTurnDone    = BIT3;
constexpr EventBits_t kBitWsClosed    = BIT4;

struct __attribute__((packed)) BinaryProto2 {
    uint16_t version;
    uint16_t type;
    uint32_t reserved;
    uint32_t timestamp;
    uint32_t payload_size;
};

struct __attribute__((packed)) BinaryProto3 {
    uint8_t type;
    uint8_t reserved;
    uint16_t payload_size;
};

struct RespBuf {
    std::string data;
};

esp_err_t OnHttpEvent(esp_http_client_event_t* evt) {
    auto* buf = static_cast<RespBuf*>(evt->user_data);
    if (evt->event_id == HTTP_EVENT_ON_DATA && buf && evt->data && evt->data_len > 0) {
        if (buf->data.size() + evt->data_len <= kMaxHttpResp) {
            buf->data.append(static_cast<const char*>(evt->data), evt->data_len);
        }
    }
    return ESP_OK;
}

std::string JStr(const cJSON* obj, const char* key, const char* def = "") {
    if (!obj) return def;
    const cJSON* it = cJSON_GetObjectItemCaseSensitive(obj, key);
    return (cJSON_IsString(it) && it->valuestring) ? std::string(it->valuestring) : std::string(def);
}

int JInt(const cJSON* obj, const char* key, int def = 0) {
    if (!obj) return def;
    const cJSON* it = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (!cJSON_IsNumber(it)) return def;
    const double d = it->valuedouble;
    if (d > 100000000.0) return 100000000;
    if (d < -100000000.0) return -100000000;
    return static_cast<int>(d);
}

int64_t NowMs() {
    return esp_timer_get_time() / 1000;
}

std::string MakeActivationUrl(const std::string& ota_url) {
    if (ota_url.empty()) return "";
    const size_t qpos = ota_url.find('?');
    std::string path = (qpos == std::string::npos) ? ota_url : ota_url.substr(0, qpos);
    const std::string query = (qpos == std::string::npos) ? "" : ota_url.substr(qpos);
    if (!path.empty() && path.back() != '/') path += '/';
    return path + "activate" + query;
}

bool HttpPostJson(const std::string& url, const std::string& body,
                  const std::string& mac, const std::string& uuid,
                  std::string* resp_out, int* status_out, std::string* err_out) {
    RespBuf buf;
    esp_http_client_config_t cfg = {};
    cfg.url = url.c_str();
    cfg.method = HTTP_METHOD_POST;
    cfg.timeout_ms = 6000;
    cfg.event_handler = OnHttpEvent;
    cfg.user_data = &buf;
    cfg.buffer_size = 2048;
    cfg.buffer_size_tx = 1024;
    if (url.rfind("https://", 0) == 0) {
        cfg.crt_bundle_attach = esp_crt_bundle_attach;
    }

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        if (err_out) *err_out = "HTTP初始化失败";
        return false;
    }

    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_header(client, "Device-Id", mac.c_str());
    esp_http_client_set_header(client, "Client-Id", uuid.c_str());
    esp_http_client_set_header(client, "User-Agent", "zectrix-note4-fridge/2.0.0");
    esp_http_client_set_post_field(client, body.c_str(), static_cast<int>(body.size()));

    esp_err_t err = esp_http_client_perform(client);
    const int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (status_out) *status_out = status;
    if (err != ESP_OK) {
        if (err_out) *err_out = std::string("连接失败: ") + esp_err_to_name(err);
        return false;
    }
    if (resp_out) *resp_out = std::move(buf.data);
    return true;
}

}  // namespace

XiaozhiClient::XiaozhiClient(FridgeStore* store) : store_(store) {
    evt_ = xEventGroupCreate();
}

XiaozhiClient::~XiaozhiClient() {
    CloseWsSession();
    if (evt_) {
        vEventGroupDelete(evt_);
        evt_ = nullptr;
    }
}

std::string XiaozhiClient::GetDeviceMac() {
    uint8_t mac[6] = {};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    char buf[24];
    std::snprintf(buf, sizeof(buf), "%02x:%02x:%02x:%02x:%02x:%02x",
                  mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return buf;
}

std::string XiaozhiClient::GetClientUuid() {
    uint8_t mac[6] = {};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    char buf[40];
    std::snprintf(buf, sizeof(buf), "00000000-0000-4000-8000-%02x%02x%02x%02x%02x%02x",
                  mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return buf;
}

std::string XiaozhiClient::RewriteLoopbackWsUrl(const std::string& ws_url,
                                                const std::string& ota_url) {
    const bool is_loopback =
        ws_url.find("://127.0.0.1") != std::string::npos ||
        ws_url.find("://localhost") != std::string::npos;
    if (!is_loopback) return ws_url;

    const size_t ota_scheme_end = ota_url.find("://");
    if (ota_scheme_end == std::string::npos) return ws_url;
    const bool ota_tls = (ota_url.rfind("https://", 0) == 0);
    const size_t ota_host_start = ota_scheme_end + 3;
    const size_t ota_path_start = ota_url.find('/', ota_host_start);
    const std::string ota_host_port =
        (ota_path_start == std::string::npos)
            ? ota_url.substr(ota_host_start)
            : ota_url.substr(ota_host_start, ota_path_start - ota_host_start);

    const size_t ws_scheme_end = ws_url.find("://");
    const size_t ws_path_start =
        (ws_scheme_end == std::string::npos)
            ? std::string::npos
            : ws_url.find('/', ws_scheme_end + 3);
    const std::string ws_path =
        (ws_path_start == std::string::npos) ? "/xiaozhi/v1/" : ws_url.substr(ws_path_start);

    return std::string(ota_tls ? "wss://" : "ws://") + ota_host_port + ws_path;
}

void XiaozhiClient::InvalidateOtaCache() {
    std::lock_guard<std::mutex> lock(mutex_);
    cached_ota_ = XiaozhiOtaInfo{};
    cached_ota_url_.clear();
}

bool XiaozhiClient::CheckOta(const AppConfig& cfg, XiaozhiOtaInfo* out, bool force_refresh) {
    const std::string url =
        cfg.ota_url.empty() ? "https://api.tenclass.net/xiaozhi/ota/" : cfg.ota_url;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!force_refresh && cached_ota_.ok && !cached_ota_.needs_activation &&
            cached_ota_url_ == url && !cached_ota_.ws_url.empty()) {
            if (out) *out = cached_ota_;
            return true;
        }
    }

    XiaozhiOtaInfo info;
    // Direct ws:// or wss:// URL bypasses HTTP OTA check completely!
    if (url.rfind("ws://", 0) == 0 || url.rfind("wss://", 0) == 0) {
        info.ok = true;
        info.ws_url = url;
        info.ws_token = cfg.ws_token;
        info.ws_version = 1;
        std::lock_guard<std::mutex> lock(mutex_);
        cached_ota_ = info;
        cached_ota_url_ = url;
        if (out) *out = info;
        return true;
    }

    const std::string mac = GetDeviceMac();
    const std::string uuid = GetClientUuid();
    cJSON* req = cJSON_CreateObject();
    cJSON* app = cJSON_AddObjectToObject(req, "application");
    cJSON_AddStringToObject(app, "version", "2.0.0");
    cJSON* board = cJSON_AddObjectToObject(req, "board");
    cJSON_AddStringToObject(board, "type", "zectrix-note4");
    cJSON_AddStringToObject(board, "name", "ZecTrix-Note4-Fridge");
    cJSON_AddStringToObject(board, "mac", mac.c_str());
    char* body_cstr = cJSON_PrintUnformatted(req);
    cJSON_Delete(req);
    const std::string body = body_cstr ? body_cstr : "{}";
    if (body_cstr) cJSON_free(body_cstr);

    std::string resp;
    int status = 0;
    std::string err;
    if (!HttpPostJson(url, body, mac, uuid, &resp, &status, &err)) {
        info.error = err;
        if (out) *out = info;
        return false;
    }
    if (status < 200 || status >= 300) {
        info.error = "语音服务HTTP " + std::to_string(status);
        if (out) *out = info;
        return false;
    }

    cJSON* root = cJSON_Parse(resp.c_str());
    if (!root) {
        info.error = "OTA响应非JSON";
        if (out) *out = info;
        return false;
    }

    if (cJSON* ws = cJSON_GetObjectItemCaseSensitive(root, "websocket"); cJSON_IsObject(ws)) {
        info.ws_url = RewriteLoopbackWsUrl(JStr(ws, "url"), url);
        info.ws_token = !cfg.ws_token.empty() ? cfg.ws_token : JStr(ws, "token");
        info.ws_version = std::max(1, JInt(ws, "version", 1));
    }
    if (cJSON* act = cJSON_GetObjectItemCaseSensitive(root, "activation"); cJSON_IsObject(act)) {
        info.activation_code = JStr(act, "code");
        info.activation_challenge = JStr(act, "challenge");
        info.activation_message = JStr(act, "message");
        info.needs_activation = !info.activation_code.empty();
    }
    cJSON_Delete(root);

    if (!info.needs_activation && info.ws_url.empty()) {
        info.error = "服务器未返回WebSocket地址";
        if (out) *out = info;
        return false;
    }

    info.ok = true;
    ESP_LOGI(kTag, "OTA ok: ws=%s ver=%d activate=%s",
             info.ws_url.c_str(), info.ws_version,
             info.needs_activation ? info.activation_code.c_str() : "no");
    {
        std::lock_guard<std::mutex> lock(mutex_);
        cached_ota_ = info;
        cached_ota_url_ = url;
    }
    if (out) *out = info;
    return true;
}

bool XiaozhiClient::PollActivate(const AppConfig& cfg, const std::string& challenge,
                                 std::string* err_out) {
    const std::string ota_url =
        cfg.ota_url.empty() ? "https://api.tenclass.net/xiaozhi/ota/" : cfg.ota_url;
    const std::string act_url = MakeActivationUrl(ota_url);
    std::string resp;
    int status = 0;
    std::string err;
    std::string body = "{}";
    if (!challenge.empty()) {
        cJSON* req = cJSON_CreateObject();
        cJSON_AddStringToObject(req, "challenge", challenge.c_str());
        char* bstr = cJSON_PrintUnformatted(req);
        cJSON_Delete(req);
        if (bstr) {
            body = bstr;
            cJSON_free(bstr);
        }
    }
    if (!HttpPostJson(act_url, body, GetDeviceMac(), GetClientUuid(), &resp, &status, &err)) {
        if (err_out) *err_out = err;
        return false;
    }
    if (status == 200) {
        InvalidateOtaCache();
        return true;
    }
    if (status == 202) {
        if (err_out) *err_out = "尚未在 xiaozhi.me 输入绑定码";
        return false;
    }
    if (err_out) *err_out = "绑定验证返回 HTTP " + std::to_string(status);
    return false;
}

bool XiaozhiClient::OpenWsSession(const XiaozhiOtaInfo& ota, std::string* err_out) {
    CloseWsSession();

    {
        std::lock_guard<std::mutex> lock(mutex_);
        ws_version_ = std::max(1, ota.ws_version);
        session_id_.clear();
        rx_text_buf_.clear();
        rx_in_text_msg_ = false;
        turn_heard_.clear();
        turn_mcp_summary_.clear();
        turn_tts_text_.clear();
        turn_error_.clear();
        turn_undo_saved_ = false;
        turn_data_changed_ = false;
        turn_timer_started_ = false;
        turn_recipe_updated_ = false;
        turn_step_delta_ = 0;
        turn_mcp_calls_ = 0;
        turn_start_ms_ = NowMs();
        turn_last_activity_ms_ = turn_start_ms_;
    }
    xEventGroupClearBits(evt_, kBitWsConnected | kBitHelloOk | kBitMcpReady |
                                   kBitTurnDone | kBitWsClosed);

    std::string headers;
    headers += "Protocol-Version: " + std::to_string(ota.ws_version) + "\r\n";
    headers += "Device-Id: " + GetDeviceMac() + "\r\n";
    headers += "Client-Id: " + GetClientUuid() + "\r\n";
    if (!ota.ws_token.empty()) {
        const std::string auth = (ota.ws_token.find(' ') == std::string::npos)
                                     ? ("Bearer " + ota.ws_token)
                                     : ota.ws_token;
        headers += "Authorization: " + auth + "\r\n";
    }

    esp_websocket_client_config_t ws_cfg = {};
    ws_cfg.uri = ota.ws_url.c_str();
    ws_cfg.headers = headers.c_str();
    ws_cfg.buffer_size = 4096;
    ws_cfg.task_stack = 12288;
    ws_cfg.network_timeout_ms = 6000;
    ws_cfg.disable_auto_reconnect = true;
    if (ota.ws_url.rfind("wss://", 0) == 0) {
        ws_cfg.crt_bundle_attach = esp_crt_bundle_attach;
    }

    ws_ = esp_websocket_client_init(&ws_cfg);
    if (!ws_) {
        if (err_out) *err_out = "WebSocket初始化失败";
        return false;
    }
    esp_websocket_register_events(ws_, WEBSOCKET_EVENT_ANY, &XiaozhiClient::WsEventHandler, this);
    esp_err_t err = esp_websocket_client_start(ws_);
    if (err != ESP_OK) {
        esp_websocket_client_destroy(ws_);
        ws_ = nullptr;
        if (err_out) *err_out = std::string("WebSocket启动失败: ") + esp_err_to_name(err);
        return false;
    }
    return true;
}

void XiaozhiClient::CloseWsSession() {
    if (ws_) {
        esp_websocket_client_stop(ws_);
        esp_websocket_client_destroy(ws_);
        ws_ = nullptr;
    }
}

void XiaozhiClient::SendTextLocked(const std::string& text) {
    if (!ws_ || !esp_websocket_client_is_connected(ws_)) return;
    esp_websocket_client_send_text(ws_, text.c_str(), static_cast<int>(text.size()),
                                   pdMS_TO_TICKS(3000));
}

void XiaozhiClient::SendOpusFrame(const uint8_t* opus_data, size_t len, uint32_t timestamp_ms) {
    if (!ws_ || !esp_websocket_client_is_connected(ws_) || !opus_data || len == 0) return;
    if (ws_version_ == 2) {
        std::vector<uint8_t> pkt(sizeof(BinaryProto2) + len);
        auto* h = reinterpret_cast<BinaryProto2*>(pkt.data());
        h->version = htons(2);
        h->type = 0;
        h->reserved = 0;
        h->timestamp = htonl(timestamp_ms);
        h->payload_size = htonl(static_cast<uint32_t>(len));
        std::memcpy(pkt.data() + sizeof(BinaryProto2), opus_data, len);
        esp_websocket_client_send_bin(ws_, reinterpret_cast<const char*>(pkt.data()),
                                      static_cast<int>(pkt.size()), pdMS_TO_TICKS(3000));
    } else if (ws_version_ == 3) {
        std::vector<uint8_t> pkt(sizeof(BinaryProto3) + len);
        auto* h = reinterpret_cast<BinaryProto3*>(pkt.data());
        h->type = 0;
        h->reserved = 0;
        h->payload_size = htons(static_cast<uint16_t>(len));
        std::memcpy(pkt.data() + sizeof(BinaryProto3), opus_data, len);
        esp_websocket_client_send_bin(ws_, reinterpret_cast<const char*>(pkt.data()),
                                      static_cast<int>(pkt.size()), pdMS_TO_TICKS(3000));
    } else {
        esp_websocket_client_send_bin(ws_, reinterpret_cast<const char*>(opus_data),
                                      static_cast<int>(len), pdMS_TO_TICKS(3000));
    }
}

void XiaozhiClient::WsEventHandler(void* arg, esp_event_base_t /*base*/,
                                   int32_t event_id, void* data) {
    static_cast<XiaozhiClient*>(arg)->HandleWsEvent(
        event_id, static_cast<esp_websocket_event_data_t*>(data));
}

void XiaozhiClient::HandleWsEvent(int32_t event_id, esp_websocket_event_data_t* data) {
    if (event_id == WEBSOCKET_EVENT_CONNECTED) {
        ESP_LOGI(kTag, "WS connected, sending hello (mcp=true)");
        xEventGroupSetBits(evt_, kBitWsConnected);
        char hello[256];
        std::snprintf(hello, sizeof(hello),
                      "{\"type\":\"hello\",\"version\":%d,\"features\":{\"mcp\":true},"
                      "\"transport\":\"websocket\","
                      "\"audio_params\":{\"format\":\"opus\",\"sample_rate\":16000,"
                      "\"channels\":1,\"frame_duration\":%d}}",
                      ws_version_, kOpusFrameMs);
        SendTextLocked(hello);
    } else if (event_id == WEBSOCKET_EVENT_DATA && data) {
        const uint8_t op = data->op_code & 0x0F;
        if (op == 0x01) {  // Text frame start (may be single or fragmented across multiple frames)
            if (data->payload_offset == 0) {
                rx_text_buf_.clear();
                rx_in_text_msg_ = true;
            }
            if (rx_in_text_msg_ && data->data_ptr && data->data_len > 0 && rx_text_buf_.size() < 32768) {
                rx_text_buf_.append(data->data_ptr, static_cast<size_t>(data->data_len));
            }
            if (rx_in_text_msg_ && data->fin &&
                data->payload_offset + data->data_len >= data->payload_len && !rx_text_buf_.empty()) {
                std::string msg = std::move(rx_text_buf_);
                rx_text_buf_.clear();
                rx_in_text_msg_ = false;
                HandleIncomingJson(msg);
            }
        } else if (op == 0x00 && rx_in_text_msg_) {  // Continuation frame of fragmented text message
            if (data->data_ptr && data->data_len > 0 && rx_text_buf_.size() < 32768) {
                rx_text_buf_.append(data->data_ptr, static_cast<size_t>(data->data_len));
            }
            if (data->fin &&
                data->payload_offset + data->data_len >= data->payload_len && !rx_text_buf_.empty()) {
                std::string msg = std::move(rx_text_buf_);
                rx_text_buf_.clear();
                rx_in_text_msg_ = false;
                HandleIncomingJson(msg);
            }
        } else if (op == 0x02) {
            // Binary audio frame; ensure stale fragmented text state is cleared if interleaved
            if (data->payload_offset == 0) rx_in_text_msg_ = false;
        }
    } else if (event_id == WEBSOCKET_EVENT_DISCONNECTED || event_id == WEBSOCKET_EVENT_ERROR) {
        ESP_LOGI(kTag, "WS closed/error id=%ld", static_cast<long>(event_id));
        if (event_id == WEBSOCKET_EVENT_ERROR) {
            std::lock_guard<std::mutex> lock(mutex_);
            if (turn_error_.empty()) turn_error_ = "语音服务器连接中断";
        }
        xEventGroupSetBits(evt_, kBitWsClosed | kBitTurnDone);
    }
}

std::string XiaozhiClient::BuildToolsListJson() const {
    return R"JSON([
{"name":"fridge_get_inventory","description":"读取冰箱当前全部食材库存与采购清单。回答冰箱有什么、或者推荐菜谱前必须先调用本工具。","inputSchema":{"type":"object","properties":{},"required":[]}},
{"name":"fridge_add_item","description":"向家庭冰箱放入或更新一样食材。若用户说了保质期几天、有效期几天或还剩几天过期，务必传入整数days；若用户完全没提保质期，不要传days。","inputSchema":{"type":"object","properties":{"name":{"type":"string","description":"食材名称，如鲜牛奶、土鸡蛋"},"qty":{"type":"string","description":"数量，如2盒、1斤(未提可省略)"},"zone":{"type":"string","description":"存放区:冷藏/冷冻/常温"},"days":{"type":"integer","description":"保质期或剩余有效天数，如7表示7天、0表示今天到期(未提及保质期时省略)"}},"required":["name"]}},
{"name":"fridge_set_expiry","description":"专门设置或修改冰箱已有食材的保质期天数/剩余天数(例如:牛奶还剩3天、酸奶保质期7天、把鸡蛋改成10天过期)，不改动原有数量和分区。","inputSchema":{"type":"object","properties":{"name":{"type":"string","description":"食材名称"},"days":{"type":"integer","description":"保质期剩余天数(0表示今天到期，3表示还剩3天)"}},"required":["name","days"]}},
{"name":"fridge_remove_item","description":"食材吃完了或用完了，从冰箱库存中移除。","inputSchema":{"type":"object","properties":{"name":{"type":"string","description":"食材名称"}},"required":["name"]}},
{"name":"fridge_add_shopping","description":"将缺的食材加入家庭采购清单。","inputSchema":{"type":"object","properties":{"name":{"type":"string","description":"采购食材名称"},"qty":{"type":"string","description":"数量，如1把"}},"required":["name"]}},
{"name":"fridge_start_timer","description":"启动厨房倒计时闹钟。","inputSchema":{"type":"object","properties":{"label":{"type":"string","description":"计时名称，如炖排骨"},"minutes":{"type":"integer","description":"分钟数"},"seconds":{"type":"integer","description":"秒数"}},"required":["minutes"]}},
{"name":"fridge_stop_timers","description":"停止或取消全部正在运行的厨房倒计时。","inputSchema":{"type":"object","properties":{},"required":[]}},
{"name":"fridge_set_recipe","description":"向墨水屏推送一道详细菜谱(3到4步，每步24字以内)，必须优先使用冰箱现有食材。","inputSchema":{"type":"object","properties":{"title":{"type":"string","description":"菜名"},"summary":{"type":"string","description":"简短卖点"},"time_min":{"type":"integer","description":"预计用时分钟"},"ingredients":{"type":"array","items":{"type":"object","properties":{"name":{"type":"string"},"amount":{"type":"string"}}}},"steps":{"type":"array","items":{"type":"object","properties":{"text":{"type":"string"},"timer_min":{"type":"integer"},"heat":{"type":"string"}}}}},"required":["title","steps"]}},
{"name":"fridge_cook_step","description":"烹饪步骤翻页，delta=1下一步，delta=-1上一步。","inputSchema":{"type":"object","properties":{"delta":{"type":"integer","description":"1下一步，-1上一步"}},"required":["delta"]}}
])JSON";
}

void XiaozhiClient::HandleMcpRequest(const void* payload_cjson) {
    const auto* payload = static_cast<const cJSON*>(payload_cjson);
    const std::string method = JStr(payload, "method");
    const cJSON* id_item = cJSON_GetObjectItemCaseSensitive(payload, "id");

    std::string sid;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        sid = session_id_;
        turn_last_activity_ms_ = NowMs();
    }

    // Properly construct & escape JSON-RPC 2.0 envelope via cJSON (preserves string/number/null id!)
    auto send_result_obj = [&](cJSON* result_obj) {
        if (!result_obj) return;
        cJSON* env = cJSON_CreateObject();
        cJSON_AddStringToObject(env, "session_id", sid.c_str());
        cJSON_AddStringToObject(env, "type", "mcp");
        cJSON* pl = cJSON_AddObjectToObject(env, "payload");
        cJSON_AddStringToObject(pl, "jsonrpc", "2.0");
        if (id_item != nullptr) {
            cJSON* id_dup = cJSON_Duplicate(id_item, true);
            if (id_dup) cJSON_AddItemToObject(pl, "id", id_dup);
        } else {
            cJSON_AddNullToObject(pl, "id");
        }
        cJSON_AddItemToObject(pl, "result", result_obj);
        char* raw = cJSON_PrintUnformatted(env);
        cJSON_Delete(env);
        if (raw) {
            SendTextLocked(raw);
            cJSON_free(raw);
        }
    };

    if (method == "initialize") {
        ESP_LOGI(kTag, "MCP initialize");
        cJSON* res = cJSON_CreateObject();
        cJSON_AddStringToObject(res, "protocolVersion", "2024-11-05");
        cJSON* caps = cJSON_AddObjectToObject(res, "capabilities");
        cJSON_AddObjectToObject(caps, "tools");
        cJSON* info = cJSON_AddObjectToObject(res, "serverInfo");
        cJSON_AddStringToObject(info, "name", "zectrix-fridge");
        cJSON_AddStringToObject(info, "version", "2.0.0");
        send_result_obj(res);
        return;
    }

    if (method == "tools/list") {
        ESP_LOGI(kTag, "MCP tools/list -> 9 fridge tools");
        cJSON* res = cJSON_CreateObject();
        cJSON* tools = cJSON_Parse(BuildToolsListJson().c_str());
        cJSON_AddItemToObject(res, "tools", tools ? tools : cJSON_CreateArray());
        send_result_obj(res);
        xEventGroupSetBits(evt_, kBitMcpReady);
        return;
    }

    if (method == "tools/call") {
        const cJSON* params = cJSON_GetObjectItemCaseSensitive(payload, "params");
        const std::string name = JStr(params, "name");
        const cJSON* args = cJSON_GetObjectItemCaseSensitive(params, "arguments");
        ESP_LOGI(kTag, "MCP tools/call: %s", name.c_str());

        auto ensure_undo = [&]() {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!turn_undo_saved_) {
                store_->SaveUndoSnapshot();
                turn_undo_saved_ = true;
            }
        };

        std::string tool_text;
        bool record_summary = true;

        if (name == "fridge_get_inventory") {
            std::string focus;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                focus = focus_ingredient_;
            }
            tool_text = store_->BuildPromptContext(focus);
            record_summary = false;  // Read-only query
        } else if (name == "fridge_add_item") {
            const std::string item_name = JStr(args, "name");
            const std::string qty = JStr(args, "qty");
            const std::string zone = JStr(args, "zone");
            const cJSON* dj = args ? cJSON_GetObjectItemCaseSensitive(args, "days") : nullptr;
            const int days = cJSON_IsNumber(dj) ? std::clamp(JInt(args, "days", 0), -365, 3650) : -999;
            if (item_name.empty()) {
                tool_text = "缺少食材名称";
            } else if (!store_->CanAddOrUpdateItem(item_name)) {
                tool_text = "冰箱库存已满(最多80样)，请先清理已吃完的食材";
            } else {
                ensure_undo();
                if (store_->AddOrUpdateItem(item_name, qty, zone, days) != 0) {
                    const FoodHint h = food_catalog::Lookup(item_name);
                    const std::string z = zone.empty() ? h.zone : zone;
                    const std::string q_disp = qty.empty() ? "" : (" " + qty);
                    const std::string exp_disp =
                        (days <= -900) ? "今天放入"
                                       : (days <= 0 ? "今天到期" : ("剩" + std::to_string(days) + "天"));
                    tool_text = "已放入" + item_name + q_disp + " (" + z + " · " + exp_disp + ")";
                    std::lock_guard<std::mutex> lock(mutex_);
                    turn_data_changed_ = true;
                } else {
                    tool_text = "冰箱库存已满(最多80样)，请先清理已吃完的食材";
                }
            }
        } else if (name == "fridge_set_expiry") {
            const std::string item_name = JStr(args, "name");
            const cJSON* dj = args ? cJSON_GetObjectItemCaseSensitive(args, "days") : nullptr;
            if (!item_name.empty() && cJSON_IsNumber(dj)) {
                const int days = std::clamp(JInt(args, "days", 0), -365, 3650);
                if (!store_->HasItemMatchingName(item_name) && !store_->CanAddOrUpdateItem(item_name)) {
                    tool_text = "冰箱库存已满(最多80样)，请先清理后再添加";
                } else {
                    ensure_undo();
                    std::string matched = store_->SetItemExpiryByName(item_name, days);
                    const std::string exp_disp =
                        days <= 0 ? "今天到期" : ("剩" + std::to_string(days) + "天");
                    if (!matched.empty()) {
                        tool_text = "已设置" + matched + "保质期：" + exp_disp;
                        std::lock_guard<std::mutex> lock(mutex_);
                        turn_data_changed_ = true;
                    } else if (store_->AddOrUpdateItem(item_name, "1份", "", days) != 0) {
                        tool_text = "已放入" + item_name + "并设保质期：" + exp_disp;
                        std::lock_guard<std::mutex> lock(mutex_);
                        turn_data_changed_ = true;
                    } else {
                        tool_text = "冰箱库存已满(最多80样)，请先清理后再添加";
                    }
                }
            } else {
                tool_text = "缺少食材名称或保质期天数";
            }
        } else if (name == "fridge_remove_item") {
            const std::string item_name = JStr(args, "name");
            if (store_->HasItemMatchingName(item_name)) {
                ensure_undo();
                const bool rm = store_->RemoveItemByName(item_name);
                tool_text = rm ? ("已从冰箱移出" + item_name) : ("冰箱里目前没有" + item_name);
                if (rm) {
                    std::lock_guard<std::mutex> lock(mutex_);
                    turn_data_changed_ = true;
                }
            } else {
                tool_text = "冰箱里目前没有" + item_name;
            }
        } else if (name == "fridge_add_shopping") {
            const std::string item_name = JStr(args, "name");
            const std::string qty = JStr(args, "qty", "1份");
            if (item_name.empty()) {
                tool_text = "缺少采购食材名";
            } else if (!store_->CanAddShoppingItem(item_name)) {
                tool_text = "采购清单已满(最多80样)，请先清理";
            } else {
                ensure_undo();
                if (store_->AddShoppingItem(item_name, qty, "语音添加") != 0) {
                    tool_text = "已加采购单:" + item_name + " " + qty;
                    std::lock_guard<std::mutex> lock(mutex_);
                    turn_data_changed_ = true;
                } else {
                    tool_text = "采购清单已满(最多80样)，请先清理";
                }
            }
        } else if (name == "fridge_start_timer") {
            const std::string label = JStr(args, "label", "厨房计时");
            const int mins = std::clamp(JInt(args, "minutes", 0), 0, 1440);
            const int secs = std::clamp(JInt(args, "seconds", 0), 0, 86400);
            if (store_->StartTimer(label, mins, secs)) {
                std::string dur_s;
                if (mins > 0) dur_s += std::to_string(mins) + "分钟";
                if (secs > 0) dur_s += std::to_string(secs) + "秒";
                tool_text = "已开启" + label + "倒计时(" + dur_s + ")";
                std::lock_guard<std::mutex> lock(mutex_);
                turn_timer_started_ = true;
            } else {
                tool_text = "计时时长无效";
            }
        } else if (name == "fridge_stop_timers") {
            const bool stopped = store_->StopAllTimers();
            tool_text = stopped ? "已停止全部厨房倒计时" : "当前没有进行中的计时";
        } else if (name == "fridge_set_recipe") {
            RecipeCard card;
            card.title = JStr(args, "title", "推荐菜谱");
            card.summary = JStr(args, "summary", "按冰箱现有食材定制");
            card.time_min = std::max(1, JInt(args, "time_min", 15));
            if (const cJSON* ia = args ? cJSON_GetObjectItemCaseSensitive(args, "ingredients") : nullptr;
                cJSON_IsArray(ia)) {
                cJSON* ig = nullptr;
                cJSON_ArrayForEach(ig, ia) {
                    if (cJSON_IsObject(ig)) {
                        const std::string n = JStr(ig, "name");
                        if (!n.empty()) card.ingredients.push_back({n, JStr(ig, "amount", "适量")});
                    } else if (cJSON_IsString(ig) && ig->valuestring) {
                        card.ingredients.push_back({ig->valuestring, "适量"});
                    }
                }
            }
            if (const cJSON* sa = args ? cJSON_GetObjectItemCaseSensitive(args, "steps") : nullptr;
                cJSON_IsArray(sa)) {
                cJSON* st = nullptr;
                cJSON_ArrayForEach(st, sa) {
                    if (cJSON_IsObject(st)) {
                        const std::string t = JStr(st, "text");
                        if (!t.empty()) card.steps.push_back({t, JInt(st, "timer_min", 0), JStr(st, "heat")});
                    } else if (cJSON_IsString(st) && st->valuestring) {
                        card.steps.push_back({st->valuestring, 0, ""});
                    }
                }
            }
            if (!card.steps.empty()) {
                ensure_undo();
                store_->PushRecipe(card);
                tool_text = "已推送菜谱: " + card.title;
                std::lock_guard<std::mutex> lock(mutex_);
                turn_recipe_updated_ = true;
                turn_data_changed_ = true;
            } else {
                tool_text = "菜谱缺少步骤";
            }
        } else if (name == "fridge_cook_step") {
            const int delta = std::clamp(JInt(args, "delta", 1), -10, 10);
            tool_text = delta >= 0 ? "已翻到烹饪下一步" : "已回到烹饪上一步";
            std::lock_guard<std::mutex> lock(mutex_);
            turn_step_delta_ += delta;
        } else {
            tool_text = "未知工具: " + name;
            record_summary = false;
        }

        if (record_summary && !tool_text.empty()) {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!turn_mcp_summary_.empty()) turn_mcp_summary_ += "；";
            turn_mcp_summary_ += tool_text;
            turn_mcp_calls_++;
        }

        // Build clean MCP tool call response JSON
        cJSON* res = cJSON_CreateObject();
        cJSON* content = cJSON_AddArrayToObject(res, "content");
        cJSON* item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "type", "text");
        cJSON_AddStringToObject(item, "text", tool_text.c_str());
        cJSON_AddItemToArray(content, item);
        cJSON_AddBoolToObject(res, "isError", false);
        send_result_obj(res);
    }
}

void XiaozhiClient::HandleIncomingJson(const std::string& text) {
    cJSON* root = cJSON_Parse(text.c_str());
    if (!root) return;
    const std::string type = JStr(root, "type");

    if (type == "hello") {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            session_id_ = JStr(root, "session_id");
            turn_last_activity_ms_ = NowMs();
        }
        ESP_LOGI(kTag, "Server hello ok, session=%s", session_id_.c_str());
        xEventGroupSetBits(evt_, kBitHelloOk);
    } else if (type == "mcp") {
        if (cJSON* payload = cJSON_GetObjectItemCaseSensitive(root, "payload");
            cJSON_IsObject(payload)) {
            HandleMcpRequest(payload);
        }
    } else if (type == "stt") {
        const std::string stt = JStr(root, "text");
        // Ignore synthetic tool-trace stt events like "% fridge_add_item"!
        if (!stt.empty() && stt.rfind("% ", 0) != 0) {
            std::lock_guard<std::mutex> lock(mutex_);
            if (turn_heard_.empty()) turn_heard_ = stt;
            turn_last_activity_ms_ = NowMs();
            ESP_LOGI(kTag, "STT heard: %s", stt.c_str());
        }
    } else if (type == "tts") {
        const std::string state = JStr(root, "state");
        const std::string txt = JStr(root, "text");
        {
            std::lock_guard<std::mutex> lock(mutex_);
            turn_last_activity_ms_ = NowMs();
            if (state == "sentence_start" && !txt.empty()) {
                if (!turn_tts_text_.empty()) turn_tts_text_ += " ";
                turn_tts_text_ += txt;
            }
        }
        if (state == "stop") {
            xEventGroupSetBits(evt_, kBitTurnDone);
        }
    }

    cJSON_Delete(root);
}

bool XiaozhiClient::BeginPttSession(const AppConfig& cfg, XiaozhiOtaInfo* ota_out) {
    std::unique_lock<std::mutex> call_lock(call_mutex_, std::try_to_lock);
    if (!call_lock.owns_lock()) {
        if (ota_out) {
            ota_out->ok = false;
            ota_out->error = "语音服务正忙，请稍候再试";
        }
        return false;
    }
    ptt_active_.store(true);
    XiaozhiOtaInfo ota;
    if (!CheckOta(cfg, &ota, false)) {
        ptt_active_.store(false);
        if (ota_out) *ota_out = ota;
        return false;
    }
    if (ota.needs_activation) {
        ptt_active_.store(false);
        if (ota_out) *ota_out = ota;
        return false;
    }
    std::string ws_err;
    if (!OpenWsSession(ota, &ws_err)) {
        // Stale cached WS URL/token after server restart: refresh OTA once automatically
        InvalidateOtaCache();
        if (!CheckOta(cfg, &ota, true) || ota.needs_activation || !OpenWsSession(ota, &ws_err)) {
            ptt_active_.store(false);
            if (!ota.needs_activation) {
                ota.ok = false;
                if (!ws_err.empty()) ota.error = ws_err;
            }
            if (ota_out) *ota_out = ota;
            return false;
        }
    }
    if (ota_out) *ota_out = ota;
    return true;
}

void XiaozhiClient::CancelPttSession() {
    std::lock_guard<std::mutex> call_lock(call_mutex_);
    CloseWsSession();
    ptt_active_.store(false);
}

XiaozhiVoiceResult XiaozhiClient::FinishPttWithWav(const std::vector<uint8_t>& wav_bytes,
                                                   const std::string& focus_ingredient) {
    std::lock_guard<std::mutex> call_lock(call_mutex_);
    struct PttGuard {
        std::atomic<bool>* flag;
        ~PttGuard() { flag->store(false); }
    } guard{&ptt_active_};
    XiaozhiVoiceResult res;
    const int64_t t0 = NowMs();

    {
        std::lock_guard<std::mutex> lock(mutex_);
        focus_ingredient_ = focus_ingredient;
    }

    if (!ws_) {
        res.error = "语音会话未建立";
        return res;
    }

    // Wait for server hello first (usually already finished while user was speaking!)
    EventBits_t bits = xEventGroupWaitBits(evt_, kBitHelloOk | kBitWsClosed,
                                           pdFALSE, pdFALSE, pdMS_TO_TICKS(5000));
    if (!(bits & kBitHelloOk)) {
        CloseWsSession();
        InvalidateOtaCache();
        res.error = "连接语音服务器超时";
        res.latency_ms = static_cast<int>(NowMs() - t0);
        return res;
    }

    // Wait briefly for server's MCP initialize + 1s + tools/list handshake (usually 0ms wait!)
    xEventGroupWaitBits(evt_, kBitMcpReady | kBitWsClosed,
                        pdFALSE, pdFALSE, pdMS_TO_TICKS(2800));
    if (xEventGroupGetBits(evt_) & kBitWsClosed) {
        CloseWsSession();
        InvalidateOtaCache();
        res.error = "语音服务器提前断开";
        res.latency_ms = static_cast<int>(NowMs() - t0);
        return res;
    }

    std::string sid;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        sid = session_id_;
    }

    // Send listen start (manual mode) safely serialized via cJSON
    {
        cJSON* ls = cJSON_CreateObject();
        cJSON_AddStringToObject(ls, "session_id", sid.c_str());
        cJSON_AddStringToObject(ls, "type", "listen");
        cJSON_AddStringToObject(ls, "state", "start");
        cJSON_AddStringToObject(ls, "mode", "manual");
        char* s = cJSON_PrintUnformatted(ls);
        cJSON_Delete(ls);
        if (s) { SendTextLocked(s); cJSON_free(s); }
    }

    // Encode recorded 16kHz 16-bit mono PCM (after 44-byte WAV header) into 60ms Opus frames!
    if (wav_bytes.size() > 44) {
        const auto* pcm = reinterpret_cast<const int16_t*>(wav_bytes.data() + 44);
        const size_t total_samples = (wav_bytes.size() - 44) / 2;
        auto encoder = std::make_unique<OpusEncoderWrapper>(16000, 1, kOpusFrameMs);
        encoder->SetComplexity(3);

        uint32_t ts_ms = 0;
        int sent_frames = 0;
        for (size_t off = 0; off < total_samples; off += kOpusFrameSamples) {
            std::vector<int16_t> frame(kOpusFrameSamples, 0);
            const size_t copy_n = std::min<size_t>(kOpusFrameSamples, total_samples - off);
            std::memcpy(frame.data(), pcm + off, copy_n * sizeof(int16_t));
            std::vector<uint8_t> opus;
            if (encoder->Encode(std::move(frame), opus) && !opus.empty()) {
                SendOpusFrame(opus.data(), opus.size(), ts_ms);
                ts_ms += kOpusFrameMs;
                if (++sent_frames % 4 == 0) vTaskDelay(pdMS_TO_TICKS(5));
            }
        }
        // Send 2 trailing silence frames (120ms) so ASR cleanly finalizes last word
        for (int s = 0; s < 2; ++s) {
            std::vector<int16_t> silence(kOpusFrameSamples, 0);
            std::vector<uint8_t> opus;
            if (encoder->Encode(std::move(silence), opus) && !opus.empty()) {
                SendOpusFrame(opus.data(), opus.size(), ts_ms);
                ts_ms += kOpusFrameMs;
            }
        }
        ESP_LOGI(kTag, "Streamed %d Opus frames (%u ms audio)", sent_frames, ts_ms);
    }

    // Send listen stop -> triggers ASR + LLM + Device MCP tool calls
    {
        cJSON* lp = cJSON_CreateObject();
        cJSON_AddStringToObject(lp, "session_id", sid.c_str());
        cJSON_AddStringToObject(lp, "type", "listen");
        cJSON_AddStringToObject(lp, "state", "stop");
        char* s = cJSON_PrintUnformatted(lp);
        cJSON_Delete(lp);
        if (s) { SendTextLocked(s); cJSON_free(s); }
    }

    // Wait up to 16s for tts stop / turn complete
    const int64_t wait_deadline = NowMs() + 16000;
    while (NowMs() < wait_deadline) {
        EventBits_t b = xEventGroupWaitBits(evt_, kBitTurnDone | kBitWsClosed,
                                            pdFALSE, pdFALSE, pdMS_TO_TICKS(500));
        if (b & (kBitTurnDone | kBitWsClosed)) break;
        std::lock_guard<std::mutex> lock(mutex_);
        if (turn_mcp_calls_ > 0 && (NowMs() - turn_last_activity_ms_) > 3000) {
            break;  // Tool already executed and server went idle
        }
    }

    CloseWsSession();

    {
        std::lock_guard<std::mutex> lock(mutex_);
        res.heard = turn_heard_;
        res.data_changed = turn_data_changed_;
        res.timer_started = turn_timer_started_;
        res.recipe_updated = turn_recipe_updated_;
        res.step_delta = turn_step_delta_;
        res.latency_ms = static_cast<int>(NowMs() - t0);

        if (!turn_mcp_summary_.empty()) {
            res.ok = true;
            res.reply = turn_mcp_summary_;
        } else if (!turn_tts_text_.empty()) {
            res.ok = true;
            res.reply = turn_tts_text_;
        } else if (!turn_heard_.empty()) {
            // Local fast-command fallback if heard phrase wasn't handled by server
            const LocalFastAction fast = store_->TryLocalFastCommand(turn_heard_);
            if (fast.handled) {
                res.ok = true;
                res.reply = fast.receipt;
                res.step_delta = fast.step_delta;
                res.timer_started = fast.switch_to_cook;
            } else {
                res.error = "已听清但服务器未返回回复";
            }
        } else {
            res.error = turn_error_.empty() ? "没听清或服务响应超时，请重试" : turn_error_;
        }
    }
    return res;
}

XiaozhiVoiceResult XiaozhiClient::SendTextCommand(const AppConfig& cfg,
                                                  const std::string& text,
                                                  const std::string& focus_ingredient) {
    XiaozhiVoiceResult res;
    std::unique_lock<std::mutex> call_lock(call_mutex_, std::try_to_lock);
    if (!call_lock.owns_lock() || ptt_active_.load()) {
        res.error = "语音服务正忙，请稍候再试";
        return res;
    }
    const int64_t t0 = NowMs();

    XiaozhiOtaInfo ota;
    if (!CheckOta(cfg, &ota, false)) {
        res.error = ota.error;
        return res;
    }
    if (ota.needs_activation) {
        res.needs_activation = true;
        res.activation_code = ota.activation_code;
        res.activation_challenge = ota.activation_challenge;
        res.error = "请先前往 xiaozhi.me 绑定 6 位码: " + ota.activation_code;
        return res;
    }

    std::string ws_err;
    if (!OpenWsSession(ota, &ws_err)) {
        res.error = ws_err;
        return res;
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        focus_ingredient_ = focus_ingredient;
        turn_heard_ = text;
    }

    EventBits_t bits = xEventGroupWaitBits(evt_, kBitHelloOk | kBitWsClosed,
                                           pdFALSE, pdFALSE, pdMS_TO_TICKS(5000));
    if (!(bits & kBitHelloOk)) {
        CloseWsSession();
        res.error = "连接语音服务器超时";
        res.latency_ms = static_cast<int>(NowMs() - t0);
        return res;
    }
    xEventGroupWaitBits(evt_, kBitMcpReady | kBitWsClosed,
                        pdFALSE, pdFALSE, pdMS_TO_TICKS(3000));

    std::string sid;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        sid = session_id_;
    }

    cJSON* msg = cJSON_CreateObject();
    cJSON_AddStringToObject(msg, "session_id", sid.c_str());
    cJSON_AddStringToObject(msg, "type", "listen");
    cJSON_AddStringToObject(msg, "state", "detect");
    cJSON_AddStringToObject(msg, "text", text.c_str());
    char* mstr = cJSON_PrintUnformatted(msg);
    cJSON_Delete(msg);
    if (mstr) {
        SendTextLocked(mstr);
        cJSON_free(mstr);
    }

    const int64_t wait_deadline = NowMs() + 16000;
    while (NowMs() < wait_deadline) {
        EventBits_t b = xEventGroupWaitBits(evt_, kBitTurnDone | kBitWsClosed,
                                            pdFALSE, pdFALSE, pdMS_TO_TICKS(500));
        if (b & (kBitTurnDone | kBitWsClosed)) break;
        std::lock_guard<std::mutex> lock(mutex_);
        if (turn_mcp_calls_ > 0 && (NowMs() - turn_last_activity_ms_) > 3000) break;
    }

    CloseWsSession();

    {
        std::lock_guard<std::mutex> lock(mutex_);
        res.heard = text;
        res.data_changed = turn_data_changed_;
        res.timer_started = turn_timer_started_;
        res.recipe_updated = turn_recipe_updated_;
        res.step_delta = turn_step_delta_;
        res.latency_ms = static_cast<int>(NowMs() - t0);
        if (!turn_mcp_summary_.empty()) {
            res.ok = true;
            res.reply = turn_mcp_summary_;
        } else if (!turn_tts_text_.empty()) {
            res.ok = true;
            res.reply = turn_tts_text_;
        } else {
            res.error = turn_error_.empty() ? "服务器未返回结果" : turn_error_;
        }
    }
    return res;
}

bool XiaozhiClient::TestConnection(const AppConfig& cfg, XiaozhiOtaInfo* ota_out,
                                   int* latency_ms_out, std::string* err_out) {
    std::unique_lock<std::mutex> call_lock(call_mutex_, std::try_to_lock);
    if (!call_lock.owns_lock() || ptt_active_.load()) {
        if (err_out) *err_out = "语音服务正忙，请稍候再试";
        return false;
    }
    const int64_t t0 = NowMs();
    XiaozhiOtaInfo ota;
    if (!CheckOta(cfg, &ota, true)) {
        if (ota_out) *ota_out = ota;
        if (latency_ms_out) *latency_ms_out = static_cast<int>(NowMs() - t0);
        if (err_out) *err_out = ota.error;
        return false;
    }
    if (ota_out) *ota_out = ota;
    if (ota.needs_activation) {
        if (latency_ms_out) *latency_ms_out = static_cast<int>(NowMs() - t0);
        if (err_out) *err_out = "需绑定6位码: " + ota.activation_code;
        return false;
    }

    std::string ws_err;
    if (!OpenWsSession(ota, &ws_err)) {
        if (latency_ms_out) *latency_ms_out = static_cast<int>(NowMs() - t0);
        if (err_out) *err_out = ws_err;
        return false;
    }
    EventBits_t bits = xEventGroupWaitBits(evt_, kBitMcpReady | kBitHelloOk | kBitWsClosed,
                                           pdFALSE, pdFALSE, pdMS_TO_TICKS(5000));
    if (bits & kBitHelloOk) {
        // Also wait up to 2s to verify MCP tools/list if server supports it
        xEventGroupWaitBits(evt_, kBitMcpReady | kBitWsClosed,
                            pdFALSE, pdFALSE, pdMS_TO_TICKS(2000));
    }
    CloseWsSession();
    if (latency_ms_out) *latency_ms_out = static_cast<int>(NowMs() - t0);
    if (!(bits & kBitHelloOk)) {
        if (err_out) *err_out = "WebSocket握手超时";
        return false;
    }
    return true;
}
