#ifndef ZECTRIX_FRIDGE_AUDIO_SERVICE_H_
#define ZECTRIX_FRIDGE_AUDIO_SERVICE_H_

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <utility>
#include <vector>

#include "zectrix_board.h"

class AudioService {
public:
    explicit AudioService(ZectrixBoard* board);

    void PlayChimeStart();
    void PlayChimeStop();
    void PlayChimeOk();
    void PlayChimeError();
    void PlayAlarmBeep();

    // True Push-to-Talk (PTT): records 16kHz 16-bit mono WAV into PSRAM
    // while `is_held()` returns true (up to max_seconds). Immune to kitchen noise!
    bool RecordPttWav(std::vector<uint8_t>* wav_out, int max_seconds,
                      const std::function<bool()>& is_held,
                      int* recorded_ms_out = nullptr);

    // Non-blocking try-lock diagnostic capture for Web UI (/api/diag/mic)
    bool TryDiagnosticCapture(std::vector<uint8_t>* wav_out, int* recorded_ms_out, bool* busy_out);

    // ---- Streaming voice playback (Xiaozhi TTS Opus frames) ----
    // TtsBegin(): a new spoken reply starts. TtsPushOpus(): one Opus packet (any thread,
    // e.g. the WebSocket task). TtsEnd(): no more packets for this reply; the queue still
    // plays out. TtsStop(): drop everything and go silent now (e.g. user pressed a key).
    void TtsBegin();
    void TtsPushOpus(const uint8_t* data, size_t len);
    void TtsEnd();
    void TtsStop();
    bool IsTtsActive() const { return tts_active_.load(); }

private:
    AudioCodec* EnsureCodecLocked();
    void PlaySequenceLocked(const std::vector<std::pair<int, int>>& notes_hz_ms, int volume = 76);
    bool RecordPttWavLocked(std::vector<uint8_t>* wav_out, int max_seconds,
                            const std::function<bool()>& is_held,
                            int* recorded_ms_out);

    static void TtsTaskEntry(void* arg);
    void TtsTaskLoop();
    // Chimes are skipped while a spoken reply is playing (they would queue behind it).
    bool LockForChime(std::unique_lock<std::mutex>* lock);

    ZectrixBoard* board_ = nullptr;
    std::mutex mutex_;  // owns the codec: chimes, recording and TTS playback

    std::mutex tts_mutex_;  // guards the fields below
    std::condition_variable tts_cv_;
    std::deque<std::vector<uint8_t>> tts_queue_;
    bool tts_input_done_ = true;
    bool tts_task_running_ = false;
    uint32_t tts_generation_ = 0;
    std::atomic<bool> tts_active_{false};
};

#endif  // ZECTRIX_FRIDGE_AUDIO_SERVICE_H_
