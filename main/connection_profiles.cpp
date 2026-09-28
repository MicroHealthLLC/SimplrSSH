/*
 * Connection Profiles Storage
 * Each record is stored as one NVS string ("p0".."p19" in the "profiles"
 * namespace, "n0".."n9" in the "wifi_nets" namespace), with fields separated by
 * the ASCII unit separator (0x1F), which cannot be typed on the keyboard and so
 * never appears inside a field.
 */

#include "connection_profiles.hpp"
#include "esp_log.h"
#include "esp_random.h"
#include "settings_nvs.hpp"
#include <cstdio>
#include <cstdlib>

static const char *TAG = "PROFILES";
static const char FIELD_SEP = '\x1F';

static std::string join_fields(const std::vector<std::string>& fields)
{
    std::string out;
    for (size_t i = 0; i < fields.size(); i++) {
        if (i > 0) {
            out += FIELD_SEP;
        }
        out += fields[i];
    }
    return out;
}

static std::vector<std::string> split_fields(const std::string& data)
{
    std::vector<std::string> fields;
    size_t start = 0;
    while (true) {
        size_t sep = data.find(FIELD_SEP, start);
        fields.push_back(data.substr(start, sep == std::string::npos ? std::string::npos : sep - start));
        if (sep == std::string::npos) {
            break;
        }
        start = sep + 1;
    }
    return fields;
}

// Reads records "<prefix>0".."<prefix>N" from an NVS namespace
static std::vector<std::string> load_records(const char* ns, char prefix, uint32_t max_count)
{
    std::vector<std::string> records;

    nvs_handle_t nvs_handle;
    esp_err_t err = settings_nvs::open(ns, NVS_READONLY, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGI(TAG, "Nothing saved in '%s' (%s)", ns, esp_err_to_name(err));
        return records;
    }

    uint32_t count = 0;
    nvs_get_u32(nvs_handle, "count", &count);

    for (uint32_t i = 0; i < count && i < max_count; i++) {
        char key[16];
        snprintf(key, sizeof(key), "%c%lu", prefix, (unsigned long)i);

        size_t required_size = 0;
        if (nvs_get_str(nvs_handle, key, NULL, &required_size) != ESP_OK) {
            continue;
        }

        std::string data(required_size, '\0');
        if (nvs_get_str(nvs_handle, key, &data[0], &required_size) != ESP_OK) {
            continue;
        }
        data.resize(required_size > 0 ? required_size - 1 : 0);  // Drop NUL terminator
        records.push_back(data);
    }

    nvs_close(nvs_handle);
    return records;
}

// Writes records and removes entries left over from a longer list (after a delete)
static esp_err_t save_records(const char* ns, char prefix, uint32_t max_count,
                              const std::vector<std::string>& records)
{
    nvs_handle_t nvs_handle;
    esp_err_t err = settings_nvs::open(ns, NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS '%s': %s", ns, esp_err_to_name(err));
        return err;
    }

    uint32_t count = records.size() > max_count ? max_count : records.size();

    for (uint32_t i = 0; i < count && err == ESP_OK; i++) {
        char key[16];
        snprintf(key, sizeof(key), "%c%lu", prefix, (unsigned long)i);
        err = nvs_set_str(nvs_handle, key, records[i].c_str());
    }

    if (err == ESP_OK) {
        err = nvs_set_u32(nvs_handle, "count", count);
    }

    if (err == ESP_OK) {
        for (uint32_t i = count; i < max_count; i++) {
            char key[16];
            snprintf(key, sizeof(key), "%c%lu", prefix, (unsigned long)i);
            nvs_erase_key(nvs_handle, key);
        }
        err = nvs_commit(nvs_handle);
    }

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to save '%s': %s", ns, esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "Saved %lu records to '%s'", (unsigned long)count, ns);
    }

    nvs_close(nvs_handle);
    return err;
}

std::string profile_store::new_id()
{
    uint32_t r;
    esp_fill_random(&r, sizeof(r));
    char id[9];
    snprintf(id, sizeof(id), "%08lx", (unsigned long)r);
    return id;
}

std::vector<ConnectionProfile> profile_store::load()
{
    std::vector<ConnectionProfile> profiles;
    bool purge_legacy = false;

    for (const auto& record : load_records("profiles", 'p', PROFILE_MAX_COUNT)) {
        std::vector<std::string> f = split_fields(record);
        ConnectionProfile p;

        if (f.size() == 9 && f[0] == "2") {
            // 2 | name | host | port | user | auth | secret_saved | key_name | id
            p.name = f[1];
            p.host = f[2];
            p.port = std::atoi(f[3].c_str());
            p.username = f[4];
            p.auth = f[5] == "k" ? ConnectionProfile::Auth::Key : ConnectionProfile::Auth::Password;
            p.secret_saved = f[6] == "1";
            p.key_name = f[7];
            p.id = f[8];
        } else if (f.size() == 8 && f[0] == "1") {
            // Pre-vault format kept the password in plain text: drop it and
            // rewrite the record so it no longer sits in flash.
            p.name = f[1];
            p.host = f[2];
            p.port = std::atoi(f[3].c_str());
            p.username = f[4];
            p.auth = f[5] == "k" ? ConnectionProfile::Auth::Key : ConnectionProfile::Auth::Password;
            p.key_name = f[7];
            p.id = new_id();
            purge_legacy = true;
        } else {
            ESP_LOGW(TAG, "Skipping unreadable profile record");
            continue;
        }

        if (!p.name.empty() && !p.host.empty()) {
            profiles.push_back(p);
        }
    }

    if (purge_legacy) {
        ESP_LOGW(TAG, "Removed plain-text passwords from old profile records");
        save(profiles);
    }

    ESP_LOGI(TAG, "Loaded %d connection profiles", (int)profiles.size());
    return profiles;
}

esp_err_t profile_store::save(const std::vector<ConnectionProfile>& profiles)
{
    std::vector<std::string> records;
    for (const auto& p : profiles) {
        records.push_back(join_fields({
            "2",
            p.name,
            p.host,
            std::to_string(p.port),
            p.username,
            p.auth == ConnectionProfile::Auth::Key ? "k" : "p",
            p.secret_saved ? "1" : "0",
            p.key_name,
            p.id,
        }));
    }
    return save_records("profiles", 'p', PROFILE_MAX_COUNT, records);
}

std::vector<SavedNetwork> network_store::load()
{
    std::vector<SavedNetwork> networks;

    for (const auto& record : load_records("wifi_nets", 'n', NETWORK_MAX_COUNT)) {
        // 2 | ssid | secret_saved | hidden   (version 1 had no hidden field)
        std::vector<std::string> f = split_fields(record);
        bool v1 = f.size() == 3 && f[0] == "1";
        bool v2 = f.size() == 4 && f[0] == "2";
        if ((!v1 && !v2) || f[1].empty()) {
            ESP_LOGW(TAG, "Skipping unreadable network record");
            continue;
        }
        SavedNetwork n;
        n.ssid = f[1];
        n.secret_saved = f[2] == "1";
        n.hidden = v2 && f[3] == "1";
        networks.push_back(n);
    }

    ESP_LOGI(TAG, "Loaded %d saved WiFi networks", (int)networks.size());
    return networks;
}

esp_err_t network_store::save(const std::vector<SavedNetwork>& networks)
{
    std::vector<std::string> records;
    for (const auto& n : networks) {
        records.push_back(join_fields({"2", n.ssid, n.secret_saved ? "1" : "0", n.hidden ? "1" : "0"}));
    }
    return save_records("wifi_nets", 'n', NETWORK_MAX_COUNT, records);
}
