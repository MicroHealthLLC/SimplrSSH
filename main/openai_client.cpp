/*
 * OpenAI Client Implementation
 */

#include "openai_client.hpp"
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "cJSON.h"
#include "esp_heap_caps.h"
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>

static const char *TAG = "OPENAI";
static const char *API = "https://api.openai.com/v1/";

// Keep answers short and plain: they are read on a 320x240 screen
static const char *SYSTEM_PROMPT =
    "You are a concise assistant on a tiny handheld with a 320x240 text screen. "
    "Answer briefly in plain text: no markdown, tables or code fences unless asked.";
static const int MAX_REPLY_TOKENS = 600;

static const size_t MAX_MODELS_RESPONSE = 256 * 1024;  // The model list is ~30 KB; kept in PSRAM
static const size_t MAX_MODELS = 40;

static esp_http_client_handle_t open_request(esp_http_client_method_t method, const char* path, const std::string& key,
                                             const char* content_type, int length)
{
    std::string url = std::string(API) + path;
    esp_http_client_config_t cfg = {};
    cfg.url = url.c_str();
    cfg.method = method;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
    cfg.timeout_ms = 60000;
    cfg.buffer_size = 1024;
    cfg.buffer_size_tx = 1024;
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        return NULL;
    }
    std::string auth = "Bearer " + key;
    esp_http_client_set_header(client, "Authorization", auth.c_str());
    if (content_type) {
        esp_http_client_set_header(client, "Content-Type", content_type);
    }
    if (esp_http_client_open(client, length) != ESP_OK) {
        esp_http_client_cleanup(client);
        return NULL;
    }
    return client;
}

static esp_http_client_handle_t open_post(const char* path, const std::string& key, const char* content_type, int length)
{
    return open_request(HTTP_METHOD_POST, path, key, content_type, length);
}

static bool write_all(esp_http_client_handle_t client, const void* data, size_t len)
{
    const char* p = (const char*)data;
    while (len > 0) {
        int n = esp_http_client_write(client, p, len);
        if (n <= 0) {
            return false;
        }
        p += n;
        len -= n;
    }
    return true;
}

// Reads the response headers; on an HTTP error puts the API's message in `error`
static bool check_status(esp_http_client_handle_t client, std::string& error)
{
    if (esp_http_client_fetch_headers(client) < 0) {
        error = "No response from OpenAI (check WiFi)";
        return false;
    }
    int status = esp_http_client_get_status_code(client);
    if (status == 200) {
        return true;
    }
    char body[512];
    int n = esp_http_client_read(client, body, sizeof(body) - 1);
    body[n > 0 ? n : 0] = '\0';
    cJSON* json = cJSON_Parse(body);
    cJSON* msg = cJSON_GetObjectItem(cJSON_GetObjectItem(json, "error"), "message");
    std::string detail = cJSON_IsString(msg) ? msg->valuestring : "";
    cJSON_Delete(json);

    if (status == 401) {
        error = "OpenAI rejected the API key" + (detail.empty() ? std::string(".") : ": " + detail.substr(0, 160));
    } else if (status == 429) {
        error = "Rate limit or no credit on the OpenAI account";
    } else {
        error = "OpenAI error " + std::to_string(status) + (detail.empty() ? "" : ": " + detail.substr(0, 160));
    }
    return false;
}

static esp_err_t finish(esp_http_client_handle_t client, bool ok)
{
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return ok ? ESP_OK : ESP_FAIL;
}

esp_err_t openai::chat(const std::string& key, const std::string& model, const std::vector<Message>& history,
                       const std::function<void(const std::string&)>& on_text, std::string& error)
{
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "model", model.c_str());
    cJSON_AddBoolToObject(root, "stream", true);
    cJSON_AddNumberToObject(root, "max_completion_tokens", MAX_REPLY_TOKENS);
    cJSON* messages = cJSON_AddArrayToObject(root, "messages");
    auto add = [messages](const char* role, const char* text) {
        cJSON* m = cJSON_CreateObject();
        cJSON_AddStringToObject(m, "role", role);
        cJSON_AddStringToObject(m, "content", text);
        cJSON_AddItemToArray(messages, m);
    };
    add("system", SYSTEM_PROMPT);
    for (const auto& m : history) {
        add(m.from_user ? "user" : "assistant", m.text.c_str());
    }
    char* body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!body) {
        error = "Out of memory";
        return ESP_ERR_NO_MEM;
    }

    esp_http_client_handle_t client = open_post("chat/completions", key, "application/json", strlen(body));
    bool ok = client && write_all(client, body, strlen(body));
    cJSON_free(body);
    if (!client) {
        error = "Can't reach OpenAI (check WiFi)";
        return ESP_FAIL;
    }
    if (!ok || !check_status(client, error)) {
        if (error.empty()) {
            error = "Connection to OpenAI failed";
        }
        return finish(client, false);
    }

    // Server-sent events: lines "data: {json}" ... "data: [DONE]"
    char buf[512];
    std::string line;
    bool done = false;
    int n;
    while (!done && (n = esp_http_client_read(client, buf, sizeof(buf))) > 0) {
        for (int i = 0; i < n && !done; i++) {
            if (buf[i] != '\n') {
                if (line.size() < 8192) {
                    line += buf[i];
                }
                continue;
            }
            if (line.rfind("data: ", 0) == 0) {
                if (line.compare(6, std::string::npos, "[DONE]") == 0) {
                    done = true;
                } else {
                    cJSON* event = cJSON_Parse(line.c_str() + 6);
                    cJSON* choice = cJSON_GetArrayItem(cJSON_GetObjectItem(event, "choices"), 0);
                    cJSON* content = cJSON_GetObjectItem(cJSON_GetObjectItem(choice, "delta"), "content");
                    if (cJSON_IsString(content) && content->valuestring[0]) {
                        on_text(content->valuestring);
                    }
                    cJSON_Delete(event);
                }
            }
            line.clear();
        }
    }
    return finish(client, true);
}

esp_err_t openai::transcribe(const std::string& key, const std::string& model, const int16_t* pcm, size_t samples,
                             uint32_t rate, std::string& text, std::string& error)
{
    static const char* B = "----pocketsshAudioBoundary";
    std::string head = std::string("--") + B + "\r\nContent-Disposition: form-data; name=\"model\"\r\n\r\n" + model + "\r\n" +
                       "--" + B + "\r\nContent-Disposition: form-data; name=\"response_format\"\r\n\r\ntext\r\n" +
                       "--" + B + "\r\nContent-Disposition: form-data; name=\"file\"; filename=\"voice.wav\"\r\n" +
                       "Content-Type: audio/wav\r\n\r\n";
    std::string tail = std::string("\r\n--") + B + "--\r\n";

    // 44-byte WAV header for 16-bit mono PCM
    uint32_t data_len = samples * 2;
    uint8_t wav[44];
    auto put32 = [&wav](int at, uint32_t v) { for (int i = 0; i < 4; i++) wav[at + i] = (v >> (8 * i)) & 0xFF; };
    auto put16 = [&wav](int at, uint16_t v) { wav[at] = v & 0xFF; wav[at + 1] = v >> 8; };
    memcpy(wav, "RIFF", 4); put32(4, 36 + data_len); memcpy(wav + 8, "WAVEfmt ", 8);
    put32(16, 16); put16(20, 1); put16(22, 1); put32(24, rate); put32(28, rate * 2); put16(32, 2); put16(34, 16);
    memcpy(wav + 36, "data", 4); put32(40, data_len);

    std::string content_type = std::string("multipart/form-data; boundary=") + B;
    int total = head.size() + sizeof(wav) + data_len + tail.size();
    esp_http_client_handle_t client = open_post("audio/transcriptions", key, content_type.c_str(), total);
    if (!client) {
        error = "Can't reach OpenAI (check WiFi)";
        return ESP_FAIL;
    }
    bool ok = write_all(client, head.data(), head.size()) && write_all(client, wav, sizeof(wav)) &&
              write_all(client, pcm, data_len) && write_all(client, tail.data(), tail.size());
    if (!ok || !check_status(client, error)) {
        if (error.empty()) {
            error = "Upload to OpenAI failed";
        }
        return finish(client, false);
    }
    char buf[256];
    int n;
    text.clear();
    while ((n = esp_http_client_read(client, buf, sizeof(buf))) > 0 && text.size() < 2000) {
        text.append(buf, n);
    }
    while (!text.empty() && (text.back() == '\n' || text.back() == ' ')) {
        text.pop_back();
    }
    return finish(client, true);
}

esp_err_t openai::speak(const std::string& key, const std::string& model, const std::string& text,
                        const std::function<void(const uint8_t*, size_t)>& on_audio, std::string& error)
{
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "model", model.c_str());
    cJSON_AddStringToObject(root, "voice", "alloy");
    cJSON_AddStringToObject(root, "response_format", "pcm");
    cJSON_AddStringToObject(root, "input", text.substr(0, 2000).c_str());
    char* body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!body) {
        return ESP_ERR_NO_MEM;
    }
    esp_http_client_handle_t client = open_post("audio/speech", key, "application/json", strlen(body));
    bool ok = client && write_all(client, body, strlen(body));
    cJSON_free(body);
    if (!client) {
        error = "Can't reach OpenAI";
        return ESP_FAIL;
    }
    if (!ok || !check_status(client, error)) {
        return finish(client, false);
    }
    uint8_t buf[1024];
    int n;
    while ((n = esp_http_client_read(client, (char*)buf, sizeof(buf) & ~1u)) > 0) {
        on_audio(buf, n);
    }
    ESP_LOGI(TAG, "Speech done");
    return finish(client, true);
}

// Raw value of "name": in one flat JSON object (string contents, or the number text)
static std::string json_field(const char* obj, const char* end, const char* name)
{
    std::string pattern = std::string("\"") + name + "\"";
    const char* p = std::search(obj, end, pattern.begin(), pattern.end());
    if (p == end) {
        return "";
    }
    p += pattern.size();
    while (p < end && (*p == ' ' || *p == ':' || *p == '\n' || *p == '\r' || *p == '\t')) {
        p++;
    }
    const char* start = p;
    if (p < end && *p == '"') {
        start = ++p;
        while (p < end && *p != '"') {
            p++;
        }
    } else {
        while (p < end && isdigit((unsigned char)*p)) {
            p++;
        }
    }
    return std::string(start, p - start);
}

// Dated snapshots (gpt-4o-2024-08-06, gpt-4-0613) duplicate the plain names
static bool is_snapshot(const std::string& id)
{
    size_t dash = id.rfind('-');
    std::string last = dash == std::string::npos ? "" : id.substr(dash + 1);
    return last.size() >= 2 && std::all_of(last.begin(), last.end(), ::isdigit);
}

static bool contains(const std::string& id, const char* part)
{
    return id.find(part) != std::string::npos;
}

// Models the Chat Completions API can talk to (not audio, image, embedding or dated snapshots)
static bool is_chat_model(const std::string& id)
{
    bool family = id.rfind("gpt-", 0) == 0 || id.rfind("chatgpt-", 0) == 0 ||
                  (id.size() > 1 && id[0] == 'o' && isdigit((unsigned char)id[1]));
    if (!family || id.size() > 60) {
        return false;
    }
    for (const char* skip : {"realtime", "audio", "transcribe", "tts", "search", "image", "instruct",
                             "embedding", "moderation", "codex", "-pro", "computer-use", "deep-research"}) {
        if (id.find(skip) != std::string::npos) {
            return false;
        }
    }
    if (is_snapshot(id)) {
        return false;
    }
    for (char c : id) {
        if (!isalnum((unsigned char)c) && c != '-' && c != '.' && c != '_' && c != ':') {
            return false;
        }
    }
    return true;
}

// Speech-to-text models that take a plain upload (not realtime or diarizing ones)
static bool is_transcribe_model(const std::string& id)
{
    return (contains(id, "transcribe") || id.rfind("whisper", 0) == 0) && !contains(id, "realtime") &&
           !contains(id, "diarize") && !is_snapshot(id);
}

static bool is_speech_model(const std::string& id)
{
    return contains(id, "tts") && !contains(id, "realtime") && !is_snapshot(id);
}

// Newest (list is newest first) whose name has "mini" - the low-cost tier - else the newest
static std::string pick_newest(const std::vector<std::string>& ids)
{
    for (const auto& id : ids) {
        if (contains(id, "mini")) {
            return id;
        }
    }
    return ids.empty() ? "" : ids.front();
}

esp_err_t openai::list_models(const std::string& key, Models& out, std::string& error)
{
    out = Models();
    std::vector<std::string>& models = out.chat;
    esp_http_client_handle_t client = open_request(HTTP_METHOD_GET, "models", key, NULL, 0);
    if (!client) {
        error = "Can't reach OpenAI (check WiFi)";
        return ESP_FAIL;
    }
    if (!check_status(client, error)) {
        return finish(client, false);
    }

    char* body = (char*)heap_caps_malloc(MAX_MODELS_RESPONSE, MALLOC_CAP_SPIRAM);
    if (!body) {
        error = "Out of memory";
        return finish(client, false);
    }
    size_t len = 0;
    int n;
    while (len < MAX_MODELS_RESPONSE && (n = esp_http_client_read(client, body + len, MAX_MODELS_RESPONSE - len)) > 0) {
        len += n;
    }
    finish(client, true);

    // {"object":"list","data":[{"id":"...","object":"model","created":123,"owned_by":"..."}, ...]}
    std::vector<std::pair<long long, std::string>> found;  // Every model, (created, id)
    const char* start = body;
    const char* end = body + len;
    const char* data = "\"data\"";
    const char* p = std::search(start, end, data, data + strlen(data));
    while (p < end && found.size() < 400) {
        const char* open = std::find(p, end, '{');
        const char* close = std::find(open, end, '}');
        if (close == end) {
            break;
        }
        std::string id = json_field(open, close, "id");
        if (!id.empty() && id.size() <= 60) {
            found.push_back({atoll(json_field(open, close, "created").c_str()), id});
        }
        p = close + 1;
    }
    heap_caps_free(body);

    std::sort(found.begin(), found.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    std::vector<std::string> transcribe, speech;
    for (const auto& f : found) {
        if (is_chat_model(f.second) && models.size() < MAX_MODELS) {
            models.push_back(f.second);
        } else if (is_transcribe_model(f.second)) {
            transcribe.push_back(f.second);
        } else if (is_speech_model(f.second)) {
            speech.push_back(f.second);
        }
    }
    out.chat_default = pick_newest(models);
    out.transcribe = pick_newest(transcribe);
    out.speech = pick_newest(speech);
    if (models.empty()) {
        error = "OpenAI listed no chat models for this key";
        return ESP_FAIL;
    }
    return ESP_OK;
}
