/*
 * OpenAI Client
 * Minimal HTTPS client for the OpenAI API (TLS verified with the built-in CA
 * bundle): the models a key can use, streamed chat completions, speech-to-text
 * transcription and text-to-speech.
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

    // What this key can use, from GET /v1/models (also checks that the key works). Defaults are
    // the newest "mini" model of each kind, else the newest, so retired models drop out by themselves.
    struct Models {
        std::vector<std::string> chat;  // Chat models, newest first
        std::string chat_default;
        std::string transcribe;         // Speech-to-text ("" if none offered)
        std::string speech;             // Text-to-speech ("" if none offered)
    };
    esp_err_t list_models(const std::string& key, Models& out, std::string& error);

    // Streams the reply: on_text is called with each new piece of text
    esp_err_t chat(const std::string& key, const std::string& model, const std::vector<Message>& history,
                   const std::function<void(const std::string&)>& on_text, std::string& error);

    // 16-bit mono PCM at `rate` Hz -> text
    esp_err_t transcribe(const std::string& key, const std::string& model, const int16_t* pcm, size_t samples,
                         uint32_t rate, std::string& text, std::string& error);

    // Text -> 24 kHz 16-bit mono PCM, streamed to on_audio
    esp_err_t speak(const std::string& key, const std::string& model, const std::string& text,
                    const std::function<void(const uint8_t*, size_t)>& on_audio, std::string& error);
}

#endif
