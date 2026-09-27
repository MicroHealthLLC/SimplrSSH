/*
 * Audio
 * T-Deck microphone (ES7210 codec on I2S0) and speaker (MAX98357A on I2S1).
 * Both are started only while in use, so they cost no RAM otherwise.
 */

#ifndef AUDIO_HPP
#define AUDIO_HPP

#include "esp_err.h"
#include <cstddef>
#include <cstdint>

namespace audio
{
    static const uint32_t MIC_RATE = 16000;      // Whisper input
    static const uint32_t SPEAKER_RATE = 24000;  // OpenAI TTS "pcm" output

    esp_err_t mic_start();
    size_t mic_read(int16_t* mono, size_t max_samples);  // Blocks for up to ~max_samples
    void mic_stop();

    esp_err_t speaker_start();
    esp_err_t speaker_write(const void* pcm, size_t bytes);
    void speaker_stop();
}

#endif
