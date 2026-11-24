#ifndef ZECTRIX_FRIDGE_XIAOZHI_CLIENT_H_
#define ZECTRIX_FRIDGE_XIAOZHI_CLIENT_H_

#include <atomic>
#include <functional>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include "esp_websocket_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "fridge_store.h"

class AudioService;

struct XiaozhiOtaInfo {
    bool ok = false;
    std::string ws_url;
    std::string ws_token;
    int ws_version = 1;
    bool needs_activation = false;
    std::string activation_code;
    std::string activation_challenge;
    std::string activation_message;
    std::string error;
};

struct XiaozhiVoiceResult {
    bool ok = false;
    bool needs_activation = false;
    std::string activation_code;
    std::string activation_challenge;
    std::string heard;
    std::string reply;
    std::string error;
    bool data_changed = false;
    bool timer_started = false;
    bool recipe_updated = false;
    int step_delta = 0;
    int latency_ms = 0;
};

// Pure single-hop Xiaozhi WebSocket + Opus + Device Fridge MCP client.
// Works seamlessly with BOTH:
//   - Public free xiaozhi.me (https://api.tenclass.net/xiaozhi/ota/) -> 0 Key, 0 cost
//   - Private LAN NAS xiaozhi-server (e.g. http://192.168.1.100:8003/xiaozhi/ota/) -> LAN instant
//   - Any custom ws:// or wss:// Xiaozhi-compatible endpoint
class XiaozhiClient {
public:
    explicit XiaozhiClient(FridgeStore* store);
    ~XiaozhiClient();

    static std::string GetDeviceMac();
    static std::string GetClientUuid();
    static std::string RewriteLoopbackWsUrl(const std::string& ws_url, const std::string& ota_url);

    // Check OTA (or pass direct ws:// / wss:// URL). Caches resolved endpoint on success.
    bool CheckOta(const AppConfig& cfg, XiaozhiOtaInfo* out, bool force_refresh = false);

    // Poll POST <ota_url>/activate after user enters the 6-digit code on xiaozhi.me.
    bool PollActivate(const AppConfig& cfg, const std::string& challenge, std::string* err_out);

    // Quick connectivity test (OTA + WS hello handshake).
    bool TestConnection(const AppConfig& cfg, XiaozhiOtaInfo* ota_out,
                        int* latency_ms_out, std::string* err_out);

    // Call right when OK hold begins so WS + MCP tools/list completes IN PARALLEL while user speaks!
    // Returns false if activation is required (fills ota_out) or OTA/WS failed.
    bool BeginPttSession(const AppConfig& cfg, XiaozhiOtaInfo* ota_out);

    // Cancel an in-progress PTT session (e.g. hold too short).
    void CancelPttSession();

    // Encode recorded 16kHz 16-bit mono WAV into 60ms Opus frames, stream to server,
    // handle all Device MCP fridge tool calls, and return final result.
    XiaozhiVoiceResult FinishPttWithWav(const std::vector<uint8_t>& wav_bytes,
                                        const std::string& focus_ingredient = "");

    // Text command through the exact same Xiaozhi + Device MCP pipeline (for phone web UI).
    XiaozhiVoiceResult SendTextCommand(const AppConfig& cfg, const std::string& text,
                                       const std::string& focus_ingredient = "");

    void InvalidateOtaCache();
    bool is_ptt_active() const { return ptt_active_.load(); }

    // Where spoken replies (server TTS) are played. nullptr = text only.
    void SetAudio(AudioService* audio) { audio_ = audio; }

    // Speak a short notice through Xiaozhi in a background task (returns at once).
    // The turn runs in "announce" mode: fridge tools are refused so nothing is changed.
    // Returns false if the voice service is busy or the task could not start.
    // on_done(spoken) runs on the announce task when the turn ends.
    bool AnnounceAsync(const AppConfig& cfg, const std::string& notice,
                       std::function<void(bool)> on_done = nullptr);
    // Cancel a pending/playing announcement (e.g. alarm silenced by a key press).
    void CancelAnnounce();
    bool is_announcing() const { return announcing_.load(); }

    // While a turn is running: what has been understood/executed so far, so the UI can show
    // the result popup right away while the spoken reply keeps playing. False = nothing yet.
    bool PeekTurnPreview(std::string* heard, std::string* summary, bool* data_changed) const;

private:
    static void WsEventHandler(void* arg, esp_event_base_t base, int32_t event_id, void* data);
    void HandleWsEvent(int32_t event_id, esp_websocket_event_data_t* data);
    void HandleIncomingJson(const std::string& text);
    void HandleMcpRequest(const void* payload_cjson);
    void SendTextLocked(const std::string& text);
    void SendOpusFrame(const uint8_t* opus_data, size_t len, uint32_t timestamp_ms);
    bool OpenWsSession(const XiaozhiOtaInfo& ota, std::string* err_out);
    void CloseWsSession();
    std::string BuildToolsListJson() const;
    bool AnnounceBlocking(const AppConfig& cfg, const std::string& notice);
    static void AnnounceTaskEntry(void* arg);
    void HandleBinaryFrame(const uint8_t* data, size_t len);
    // Wait for the server to finish the turn (tts stop), keeping the socket open while
    // its spoken reply is still streaming in.
    void WaitTurnEnd();

    FridgeStore* store_ = nullptr;
    mutable std::mutex mutex_;
    std::mutex call_mutex_;
    std::atomic<bool> ptt_active_{false};

    XiaozhiOtaInfo cached_ota_;
    std::string cached_ota_url_;

    esp_websocket_client_handle_t ws_ = nullptr;
    EventGroupHandle_t evt_ = nullptr;
    int ws_version_ = 1;
    std::string session_id_;
    std::string rx_text_buf_;
    bool rx_in_text_msg_ = false;
    std::vector<uint8_t> rx_bin_buf_;
    bool rx_in_bin_msg_ = false;

    AudioService* audio_ = nullptr;
    std::atomic<bool> announcing_{false};
    std::atomic<bool> announce_cancel_{false};
    std::string focus_ingredient_;

    // Per-turn collected results (protected by mutex_)
    std::string turn_heard_;
    std::string turn_mcp_summary_;
    std::string turn_tts_text_;
    std::string turn_error_;
    bool turn_undo_saved_ = false;
    bool turn_data_changed_ = false;
    bool turn_timer_started_ = false;
    bool turn_recipe_updated_ = false;
    int turn_step_delta_ = 0;
    int turn_mcp_calls_ = 0;
    int64_t turn_last_activity_ms_ = 0;
    int64_t turn_start_ms_ = 0;
};

#endif  // ZECTRIX_FRIDGE_XIAOZHI_CLIENT_H_
