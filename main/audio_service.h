#ifndef ZECTRIX_FRIDGE_AUDIO_SERVICE_H_
#define ZECTRIX_FRIDGE_AUDIO_SERVICE_H_

#include <cstdint>
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

private:
    AudioCodec* EnsureCodecLocked();
    void PlaySequenceLocked(const std::vector<std::pair<int, int>>& notes_hz_ms, int volume = 76);
    bool RecordPttWavLocked(std::vector<uint8_t>* wav_out, int max_seconds,
                            const std::function<bool()>& is_held,
                            int* recorded_ms_out);

    ZectrixBoard* board_ = nullptr;
    std::mutex mutex_;
};

#endif  // ZECTRIX_FRIDGE_AUDIO_SERVICE_H_
