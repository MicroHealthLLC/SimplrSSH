/*
 * OpenAI Client
 * Minimal HTTPS client for the OpenAI API (TLS verified with the built-in CA
 * bundle): streamed chat completions, Whisper transcription and text-to-speech.
 * Blocking; call from a worker task, never from the UI task.
 */

#ifndef OPENAI_CLIENT_HPP
#define OPENAI_CLIENT_HPP

#include "esp_err.h"
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace openai
{
    struct Message {
        bool from_user;
        std::string text;
    };

    // Streams the reply: on_text is called with each new piece of text
    esp_err_t chat(const std::string& key, const std::string& model, const std::vector<Message>& history,
                   const std::function<void(const std::string&)>& on_text, std::string& error);

    // 16-bit mono PCM at `rate` Hz -> text (Whisper)
    esp_err_t transcribe(const std::string& key, const int16_t* pcm, size_t samples, uint32_t rate,
                         std::string& text, std::string& error);

    // Text -> 24 kHz 16-bit mono PCM, streamed to on_audio
    esp_err_t speak(const std::string& key, const std::string& text,
                    const std::function<void(const uint8_t*, size_t)>& on_audio, std::string& error);
}

#endif
