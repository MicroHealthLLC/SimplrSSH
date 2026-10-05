/*
 * Audio
 * Board-independent processing of recordings. The microphone and speaker drivers are
 * per board: audio_tdeck.cpp, audio_tab5.cpp.
 */

#include "audio.hpp"
#include <algorithm>

int audio::normalize(int16_t* mono, size_t samples)
{
    if (samples == 0) {
        return 0;
    }
    int64_t sum = 0;
    for (size_t i = 0; i < samples; i++) {
        sum += mono[i];
    }
    int32_t dc = (int32_t)(sum / (int64_t)samples);
    int32_t peak = 0;
    for (size_t i = 0; i < samples; i++) {
        int32_t v = mono[i] - dc;
        peak = std::max(peak, v < 0 ? -v : v);
    }
    // Target about half of full scale; Whisper copes poorly with very quiet input
    int32_t gain_x16 = peak > 0 ? std::min<int32_t>(16 * 16, 16 * 16000 / peak) : 16;
    gain_x16 = std::max<int32_t>(gain_x16, 16);
    for (size_t i = 0; i < samples; i++) {
        int32_t v = (mono[i] - dc) * gain_x16 / 16;
        mono[i] = (int16_t)std::min<int32_t>(32767, std::max<int32_t>(-32768, v));
    }
    return (int)std::min<int32_t>(peak, 32767);
}
