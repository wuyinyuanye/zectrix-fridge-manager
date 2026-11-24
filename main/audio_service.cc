#include "audio_service.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "audio_codec.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace {

constexpr char kTag[] = "audio_svc";
constexpr int kSampleRate = 16000;
constexpr double kPi = 3.14159265358979323846;

void WriteLe16(uint8_t* p, uint16_t v) {
    p[0] = static_cast<uint8_t>(v & 0xFF);
    p[1] = static_cast<uint8_t>((v >> 8) & 0xFF);
}

void WriteLe32(uint8_t* p, uint32_t v) {
    p[0] = static_cast<uint8_t>(v & 0xFF);
    p[1] = static_cast<uint8_t>((v >> 8) & 0xFF);
    p[2] = static_cast<uint8_t>((v >> 16) & 0xFF);
    p[3] = static_cast<uint8_t>((v >> 24) & 0xFF);
}

void BuildWavHeader(uint8_t hdr[44], uint32_t pcm_bytes) {
    std::memcpy(hdr, "RIFF", 4);
    WriteLe32(hdr + 4, 36 + pcm_bytes);
    std::memcpy(hdr + 8, "WAVE", 4);
    std::memcpy(hdr + 12, "fmt ", 4);
    WriteLe32(hdr + 16, 16);
    WriteLe16(hdr + 20, 1);
    WriteLe16(hdr + 22, 1);
    WriteLe32(hdr + 24, kSampleRate);
    WriteLe32(hdr + 28, kSampleRate * 2);
    WriteLe16(hdr + 32, 2);
    WriteLe16(hdr + 34, 16);
    std::memcpy(hdr + 36, "data", 4);
    WriteLe32(hdr + 40, pcm_bytes);
}

}  // namespace

AudioService::AudioService(ZectrixBoard* board) : board_(board) {}

AudioCodec* AudioService::EnsureCodecLocked() {
    if (board_ == nullptr) return nullptr;
    board_->SetAudioPower(true);
    AudioCodec* codec = board_->PrepareAudio();
    if (codec == nullptr || !codec->valid()) {
        ESP_LOGW(kTag, "ES8311 unavailable");
        return nullptr;
    }
    return codec;
}

void AudioService::PlaySequenceLocked(const std::vector<std::pair<int, int>>& notes_hz_ms,
                                      int volume) {
    AudioCodec* codec = EnsureCodecLocked();
    if (codec == nullptr) return;

    // Close input first so speaker chime audio NEVER pollutes the I2S RX DMA buffer
    codec->EnableInput(false);
    codec->SetOutputVolume(volume);
    codec->EnableOutput(true);
    vTaskDelay(pdMS_TO_TICKS(20));

    std::vector<int16_t> pcm;
    for (const auto& [freq, ms] : notes_hz_ms) {
        const int count = (kSampleRate * ms) / 1000;
        const int fade = std::min(count / 5, 120);
        for (int i = 0; i < count; ++i) {
            if (freq <= 0) { pcm.push_back(0); continue; }
            double env = 1.0;
            if (i < fade) env = static_cast<double>(i) / fade;
            else if (i > count - fade) env = static_cast<double>(count - i) / fade;
            pcm.push_back(static_cast<int16_t>(
                std::sin(2.0 * kPi * freq * i / kSampleRate) * 8500.0 * env));
        }
    }
    if (!pcm.empty()) codec->OutputData(pcm);
    // OutputData() returns as soon as the samples are queued in the I2S DMA ring
    // (6 x 240 frames = 90ms). Wait for that queue to finish playing before muting,
    // otherwise short chimes (<90ms) are cut off before they are ever heard.
    const int total_ms = static_cast<int>((pcm.size() * 1000) / kSampleRate);
    constexpr int kDmaQueueMs = (AUDIO_CODEC_DMA_DESC_NUM * AUDIO_CODEC_DMA_FRAME_NUM * 1000) / kSampleRate;
    vTaskDelay(pdMS_TO_TICKS(std::min(total_ms, kDmaQueueMs) + 30));
    codec->EnableOutput(false);
}

void AudioService::PlayChimeStart() {
    std::lock_guard<std::mutex> lock(mutex_);
    // Rising "ding-ding": start speaking now
    PlaySequenceLocked({{784, 80}, {0, 20}, {1175, 120}}, 76);
}
void AudioService::PlayChimeStop() {
    std::lock_guard<std::mutex> lock(mutex_);
    // Falling "dong": recording finished, sending to Xiaozhi
    PlaySequenceLocked({{988, 80}, {0, 20}, {659, 130}}, 74);
}
void AudioService::PlayChimeOk() {
    std::lock_guard<std::mutex> lock(mutex_);
    PlaySequenceLocked({{587, 65}, {880, 65}, {1175, 100}}, 76);
}
void AudioService::PlayChimeError() {
    std::lock_guard<std::mutex> lock(mutex_);
    PlaySequenceLocked({{392, 95}, {0, 30}, {294, 130}}, 76);
}
void AudioService::PlayAlarmBeep() {
    std::lock_guard<std::mutex> lock(mutex_);
    PlaySequenceLocked({{1046, 110}, {0, 55}, {1318, 110}, {0, 55}, {1568, 160}}, 88);
}

bool AudioService::RecordPttWav(std::vector<uint8_t>* wav_out, int max_seconds,
                                const std::function<bool()>& is_held,
                                int* recorded_ms_out) {
    std::lock_guard<std::mutex> lock(mutex_);
    return RecordPttWavLocked(wav_out, max_seconds, is_held, recorded_ms_out);
}

bool AudioService::TryDiagnosticCapture(std::vector<uint8_t>* wav_out,
                                        int* recorded_ms_out, bool* busy_out) {
    if (busy_out) *busy_out = false;
    std::unique_lock<std::mutex> lock(mutex_, std::try_to_lock);
    if (!lock.owns_lock()) {
        if (busy_out) *busy_out = true;
        return false;
    }
    PlaySequenceLocked({{880, 55}}, 72);
    int calls = 0;
    return RecordPttWavLocked(wav_out, 2, [&calls]() { return ++calls < 55; }, recorded_ms_out);
}

bool AudioService::RecordPttWavLocked(std::vector<uint8_t>* wav_out, int max_seconds,
                                      const std::function<bool()>& is_held,
                                      int* recorded_ms_out) {
    if (wav_out == nullptr) return false;
    wav_out->clear();

    AudioCodec* codec = EnsureCodecLocked();
    if (codec == nullptr) return false;

    // Reset codec cleanly so SetInputGain(36.0f) is applied on esp_codec_dev_open()
    // and zero chime echo remains in the I2S RX DMA buffer!
    codec->EnableOutput(false);
    codec->EnableInput(false);
    vTaskDelay(pdMS_TO_TICKS(25));
    codec->SetInputGain(36.0f);
    codec->EnableInput(true);
    vTaskDelay(pdMS_TO_TICKS(40));

    // Discard initial ADC power-up transient (~80ms)
    std::vector<int16_t> chunk(320);  // 20ms @ 16kHz
    for (int i = 0; i < 4; ++i) codec->InputData(chunk);

    const int max_chunks = std::max(50, max_seconds * 50);
    std::vector<int16_t> pcm;
    pcm.reserve(static_cast<size_t>(max_chunks) * chunk.size());

    // Record cleanly while OK button is held, plus 240ms tail so final syllable isn't clipped
    int tail_chunks = 0;
    bool released = false;
    for (int c = 0; c < max_chunks; ++c) {
        if (!codec->InputData(chunk)) break;
        pcm.insert(pcm.end(), chunk.begin(), chunk.end());

        if (!released && is_held && !is_held()) {
            released = true;
            tail_chunks = 12;  // 240ms tail
        }
        if (released) {
            if (--tail_chunks <= 0) break;
        }
    }

    codec->EnableInput(false);

    const int recorded_ms = static_cast<int>((pcm.size() * 1000) / kSampleRate);
    if (recorded_ms_out) *recorded_ms_out = recorded_ms;
    if (recorded_ms < 260) {
        ESP_LOGW(kTag, "PTT recording too short (%d ms)", recorded_ms);
        return false;
    }

    int32_t peak = 0;
    int64_t sq_sum = 0;
    for (int16_t s : pcm) {
        const int32_t v = s < 0 ? -static_cast<int32_t>(s) : static_cast<int32_t>(s);
        if (v > peak) peak = v;
        sq_sum += static_cast<int64_t>(s) * s;
    }
    const int rms = pcm.empty() ? 0 : static_cast<int>(std::sqrt(static_cast<double>(sq_sum) / pcm.size()));

    const uint32_t pcm_bytes = static_cast<uint32_t>(pcm.size() * sizeof(int16_t));
    wav_out->resize(44 + pcm_bytes);
    BuildWavHeader(wav_out->data(), pcm_bytes);
    std::memcpy(wav_out->data() + 44, pcm.data(), pcm_bytes);
    ESP_LOGI(kTag, "PTT WAV captured: %u bytes (%d ms), peak=%ld rms=%d",
             static_cast<unsigned>(wav_out->size()), recorded_ms, static_cast<long>(peak), rms);
    return true;
}
