#include "audio_service.h"

#include <algorithm>
#include <cmath>
#include <chrono>
#include <cstring>

#include "audio_codec.h"
#include "esp_heap_caps.h"
#include "freertos/idf_additions.h"
#include "opus_decoder.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace {

constexpr char kTag[] = "audio_svc";
constexpr int kSampleRate = 16000;
constexpr double kPi = 3.14159265358979323846;
// ES8311 ADC_REG16 supports 6 dB steps (0..42 dB) on top of the fixed +30 dB analog PGA.
// 36 dB clipped near-field speech at peak=32768; 30 dB matches Es8311AudioCodec's default
// and gives 6 dB more headroom while keeping fridge-distance speech clear.
constexpr float kMicInputGainDb = 30.0f;
constexpr int kTtsVolume = 70;
constexpr size_t kTtsMaxQueuedPackets = 500;   // ~30s of 60ms frames
constexpr int kTtsIdleGapMs = 4000;            // stop if the stream stalls this long
// Opus decoding uses alloca heavily: give it a roomy stack, placed in PSRAM.
constexpr uint32_t kTtsTaskStack = 32768;

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
    std::unique_lock<std::mutex> lock;
    if (!LockForChime(&lock)) return;
    PlaySequenceLocked({{587, 65}, {880, 65}, {1175, 100}}, 76);
}
void AudioService::PlayChimeError() {
    std::unique_lock<std::mutex> lock;
    if (!LockForChime(&lock)) return;
    PlaySequenceLocked({{392, 95}, {0, 30}, {294, 130}}, 76);
}
void AudioService::PlayAlarmBeep() {
    std::unique_lock<std::mutex> lock;
    if (!LockForChime(&lock)) return;
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

    // Reset codec cleanly so SetInputGain(kMicInputGainDb) is applied on esp_codec_dev_open()
    // and zero chime echo remains in the I2S RX DMA buffer!
    codec->EnableOutput(false);
    codec->EnableInput(false);
    vTaskDelay(pdMS_TO_TICKS(25));
    codec->SetInputGain(kMicInputGainDb);
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
    int clipped = 0;
    for (int16_t s : pcm) {
        const int32_t v = s < 0 ? -static_cast<int32_t>(s) : static_cast<int32_t>(s);
        if (v > peak) peak = v;
        if (v >= 32700) ++clipped;
        sq_sum += static_cast<int64_t>(s) * s;
    }
    const int rms = pcm.empty() ? 0 : static_cast<int>(std::sqrt(static_cast<double>(sq_sum) / pcm.size()));

    const uint32_t pcm_bytes = static_cast<uint32_t>(pcm.size() * sizeof(int16_t));
    wav_out->resize(44 + pcm_bytes);
    BuildWavHeader(wav_out->data(), pcm_bytes);
    std::memcpy(wav_out->data() + 44, pcm.data(), pcm_bytes);
    ESP_LOGI(kTag, "PTT WAV captured: %u bytes (%d ms), gain=%.0fdB peak=%ld rms=%d clip=%d",
             static_cast<unsigned>(wav_out->size()), recorded_ms, kMicInputGainDb,
             static_cast<long>(peak), rms, clipped);
    return true;
}

// ============================================================================
// Streaming TTS playback
// ============================================================================
bool AudioService::LockForChime(std::unique_lock<std::mutex>* lock) {
    *lock = std::unique_lock<std::mutex>(mutex_, std::try_to_lock);
    if (lock->owns_lock()) return true;
    if (tts_active_.load()) return false;  // speaking: don't queue a beep behind the voice
    lock->lock();
    return true;
}

void AudioService::TtsBegin() {
    std::lock_guard<std::mutex> lk(tts_mutex_);
    tts_input_done_ = false;
    tts_active_.store(true);
}

void AudioService::TtsPushOpus(const uint8_t* data, size_t len) {
    if (data == nullptr || len == 0) return;
    bool start_task = false;
    uint32_t gen = 0;
    {
        std::lock_guard<std::mutex> lk(tts_mutex_);
        if (tts_queue_.size() >= kTtsMaxQueuedPackets) return;
        tts_queue_.emplace_back(data, data + len);
        tts_active_.store(true);
        if (!tts_task_running_) {
            tts_task_running_ = true;
            start_task = true;
            gen = tts_generation_;
        }
    }
    tts_cv_.notify_one();
    if (start_task) {
        if (xTaskCreateWithCaps(&AudioService::TtsTaskEntry, "tts_play", kTtsTaskStack, this, 5,
                                nullptr, MALLOC_CAP_SPIRAM) != pdPASS) {
            ESP_LOGE(kTag, "could not start TTS playback task");
            std::lock_guard<std::mutex> lk(tts_mutex_);
            if (tts_generation_ == gen) {
                tts_queue_.clear();
                tts_task_running_ = false;
                tts_active_.store(false);
            }
        }
    }
}

void AudioService::TtsEnd() {
    {
        std::lock_guard<std::mutex> lk(tts_mutex_);
        tts_input_done_ = true;
        if (!tts_task_running_ && tts_queue_.empty()) tts_active_.store(false);
    }
    tts_cv_.notify_one();
}

void AudioService::TtsStop() {
    {
        std::lock_guard<std::mutex> lk(tts_mutex_);
        tts_queue_.clear();
        tts_input_done_ = true;
        ++tts_generation_;
        if (!tts_task_running_) tts_active_.store(false);
    }
    tts_cv_.notify_one();
    // Wait (briefly) for the player to release the codec so the caller can use it.
    for (int i = 0; i < 40 && tts_active_.load(); ++i) vTaskDelay(pdMS_TO_TICKS(10));
}

void AudioService::TtsTaskEntry(void* arg) {
    static_cast<AudioService*>(arg)->TtsTaskLoop();
    vTaskDeleteWithCaps(nullptr);
}

void AudioService::TtsTaskLoop() {
    std::unique_lock<std::mutex> codec_lock(mutex_);
    uint32_t my_gen = 0;
    {
        std::lock_guard<std::mutex> lk(tts_mutex_);
        my_gen = tts_generation_;
    }
    AudioCodec* codec = EnsureCodecLocked();
    // Decode straight to the codec's 16kHz rate (Opus can decode any stream at 16kHz).
    // 120ms buffer covers every frame duration the server may choose.
    OpusDecoderWrapper decoder(kSampleRate, 1, 120);
    if (codec != nullptr) {
        codec->EnableInput(false);
        codec->SetOutputVolume(kTtsVolume);
        codec->EnableOutput(true);
    }

    int played_ms = 0;
    std::vector<int16_t> pcm;
    while (true) {
        std::vector<uint8_t> pkt;
        {
            std::unique_lock<std::mutex> lk(tts_mutex_);
            const bool got = tts_cv_.wait_for(lk, std::chrono::milliseconds(kTtsIdleGapMs), [&] {
                return !tts_queue_.empty() || tts_input_done_ || tts_generation_ != my_gen;
            });
            if (tts_generation_ != my_gen) break;                 // TtsStop()
            if (tts_queue_.empty() && (tts_input_done_ || !got)) break;  // finished / stalled
            if (tts_queue_.empty()) continue;
            pkt = std::move(tts_queue_.front());
            tts_queue_.pop_front();
        }
        if (codec == nullptr) continue;
        if (decoder.Decode(std::move(pkt), pcm) && !pcm.empty()) {
            codec->OutputData(pcm);
            played_ms += static_cast<int>(pcm.size() * 1000 / kSampleRate);
        }
    }

    if (codec != nullptr) {
        // Let the DMA ring play out before muting (same reason as PlaySequenceLocked)
        constexpr int kDmaQueueMs =
            (AUDIO_CODEC_DMA_DESC_NUM * AUDIO_CODEC_DMA_FRAME_NUM * 1000) / kSampleRate;
        vTaskDelay(pdMS_TO_TICKS(kDmaQueueMs + 30));
        codec->EnableOutput(false);
    }
    ESP_LOGI(kTag, "TTS played %d ms (stack free %u bytes)", played_ms,
             static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));

    std::lock_guard<std::mutex> lk(tts_mutex_);
    tts_task_running_ = false;
    // TtsStop() already cleared the old reply; anything queued now belongs to a new one.
    tts_active_.store(!tts_queue_.empty());
    codec_lock.unlock();
    if (!tts_queue_.empty()) {
        // A new reply started while we were finishing: hand over to a fresh task
        tts_task_running_ = true;
        if (xTaskCreateWithCaps(&AudioService::TtsTaskEntry, "tts_play", kTtsTaskStack, this, 5,
                                nullptr, MALLOC_CAP_SPIRAM) != pdPASS) {
            tts_task_running_ = false;
            tts_queue_.clear();
            tts_active_.store(false);
        }
    }
}
