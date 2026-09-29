/*
 * Settings Backup Implementation
 *
 * settings.dat:
 *   SIMPLRSSH-BACKUP 1 <profiles> <networks> <hosts> <history>
 *   (backups made before the rename start with POCKETSSH-BACKUP 1 and are still read)
 *   <namespace> <key> s <hex string value>
 *   <namespace> <key> u <decimal u32 value>
 *   <namespace> <key> b <hex blob>            (encrypted vault records)
 *
 * Saved passwords are copied as the encrypted "vault" records. The per-device
 * secret that unlocks them ("vault_dev") is never backed up, so without a PIN a
 * backup's passwords only work on the device that made it.
 *
 * The backup comes from removable media, so restore treats it as untrusted:
 * only known namespaces, NVS-sized keys and values are accepted, and the whole
 * file is parsed before the device is modified.
 */

#include "settings_backup.hpp"
#include "secret_vault.hpp"
#include "esp_log.h"
#include "settings_nvs.hpp"
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cctype>
#include <vector>
#include <sys/stat.h>

static const char *TAG = "BACKUP";

static const char* const NAMESPACES[] = {"profiles", "wifi_nets", "known_hosts", "storage", "vault", "chat"};
static const char* const HEADER = "SIMPLRSSH-BACKUP 1";
static const char* const LEGACY_HEADER = "POCKETSSH-BACKUP 1";   // Same format, older name
static const size_t MAX_LINE = 8300;     // "ns key s " + hex of a 4000-byte string
static const size_t MAX_RECORDS = 1000;
static const size_t MAX_BLOB = 1024;

struct Record {
    std::string ns;
    std::string key;
    char type;           // 's' string, 'u' u32, 'b' blob
    std::string value;   // String / blob bytes, or decimal for 'u'
};

static bool known_namespace(const std::string& ns)
{
    for (const char* n : NAMESPACES) {
        if (ns == n) {
            return true;
        }
    }
    return false;
}

static bool valid_key(const std::string& key)
{
    if (key.empty() || key.size() > 15) {
        return false;
    }
    for (char c : key) {
        if (!isalnum((unsigned char)c) && c != '_') {
            return false;
        }
    }
    return true;
}

static std::string to_hex(const std::string& s)
{
    static const char digits[] = "0123456789abcdef";
    std::string out;
    out.reserve(s.size() * 2);
    for (unsigned char c : s) {
        out += digits[c >> 4];
        out += digits[c & 0xF];
    }
    return out;
}

static bool from_hex(const std::string& hex, std::string& out, bool allow_nul = false)
{
    if (hex.size() % 2 != 0) {
        return false;
    }
    out.clear();
    for (size_t i = 0; i < hex.size(); i += 2) {
        if (!isxdigit((unsigned char)hex[i]) || !isxdigit((unsigned char)hex[i + 1])) {
            return false;
        }
        char c = (char)strtol(hex.substr(i, 2).c_str(), NULL, 16);
        if (c == '\0' && !allow_nul) {
            return false;
        }
        out += c;
    }
    return true;
}

static std::vector<Record> read_device_records()
{
    std::vector<Record> records;
    for (const char* ns : NAMESPACES) {
        nvs_handle_t handle;
        if (settings_nvs::open(ns, NVS_READONLY, &handle) != ESP_OK) {
            continue;
        }
        nvs_iterator_t it = NULL;
        esp_err_t err = nvs_entry_find(settings_nvs::partition(), ns, NVS_TYPE_ANY, &it);
        while (err == ESP_OK) {
            nvs_entry_info_t info;
            nvs_entry_info(it, &info);
            if (info.type == NVS_TYPE_STR) {
                size_t len = 0;
                if (nvs_get_str(handle, info.key, NULL, &len) == ESP_OK && len > 0) {
                    std::string value(len, '\0');
                    if (nvs_get_str(handle, info.key, &value[0], &len) == ESP_OK) {
                        value.resize(len - 1);
                        records.push_back({ns, info.key, 's', value});
                    }
                }
            } else if (info.type == NVS_TYPE_BLOB) {
                size_t len = 0;
                if (nvs_get_blob(handle, info.key, NULL, &len) == ESP_OK && len > 0 && len <= MAX_BLOB) {
                    std::string value(len, '\0');
                    if (nvs_get_blob(handle, info.key, &value[0], &len) == ESP_OK) {
                        records.push_back({ns, info.key, 'b', value});
                    }
                }
            } else if (info.type == NVS_TYPE_U32) {
                uint32_t v = 0;
                if (nvs_get_u32(handle, info.key, &v) == ESP_OK) {
                    records.push_back({ns, info.key, 'u', std::to_string(v)});
                }
            }
            err = nvs_entry_next(&it);
        }
        nvs_release_iterator(it);
        nvs_close(handle);
    }
    return records;
}

static settings_backup::Counts count_records(const std::vector<Record>& records)
{
    settings_backup::Counts c;
    for (const auto& r : records) {
        if (r.ns == "profiles" && r.key[0] == 'p') {
            c.profiles++;
        } else if (r.ns == "wifi_nets" && r.key[0] == 'n') {
            c.networks++;
        } else if (r.ns == "known_hosts") {
            c.hosts++;
        } else if (r.ns == "storage" && r.key.rfind("hist_", 0) == 0 && r.key != "hist_count") {
            c.history++;
        } else if (r.ns == "vault" && r.key[0] == 's') {
            c.vault_files++;
        }
    }
    return c;
}

settings_backup::Counts settings_backup::count_device()
{
    return count_records(read_device_records());
}

static std::string path_in(const std::string& dir, const char* name)
{
    return dir + "/" + name;
}

esp_err_t settings_backup::backup(const std::string& dir, Counts& counts)
{
    mkdir(dir.c_str(), 0755);

    std::vector<Record> records = read_device_records();
    counts = count_records(records);

    FILE* f = fopen(path_in(dir, "settings.new").c_str(), "w");
    if (!f) {
        ESP_LOGE(TAG, "Cannot write to %s", dir.c_str());
        return ESP_FAIL;
    }
    bool ok = fprintf(f, "%s %d %d %d %d\n", HEADER, counts.profiles, counts.networks,
                      counts.hosts, counts.history) > 0;
    for (const auto& r : records) {
        std::string value = r.type == 'u' ? r.value : to_hex(r.value);
        ok = ok && fprintf(f, "%s %s %c %s\n", r.ns.c_str(), r.key.c_str(), r.type, value.c_str()) > 0;
    }
    ok = (fclose(f) == 0) && ok;

    if (!ok) {
        remove(path_in(dir, "settings.new").c_str());
        return ESP_FAIL;
    }

    // Replace the previous backup only once the new one is complete
    remove(path_in(dir, "settings.dat").c_str());
    remove(path_in(dir, "vault.dat").c_str());  // Older backup format
    if (rename(path_in(dir, "settings.new").c_str(), path_in(dir, "settings.dat").c_str()) != 0) {
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Backup written: %d profiles, %d networks, %d hosts", counts.profiles, counts.networks, counts.hosts);
    return ESP_OK;
}

// Length of the header the line starts with (current or legacy), or 0 if neither
static size_t header_length(const std::string& line)
{
    if (line.rfind(HEADER, 0) == 0) {
        return strlen(HEADER);
    }
    if (line.rfind(LEGACY_HEADER, 0) == 0) {
        return strlen(LEGACY_HEADER);
    }
    return 0;
}

static bool read_line(FILE* f, std::string& line)
{
    line.clear();
    int c;
    while ((c = fgetc(f)) != EOF) {
        if (c == '\n') {
            return true;
        }
        if (line.size() >= MAX_LINE) {
            return false;
        }
        line += (char)c;
    }
    return !line.empty();
}

bool settings_backup::read_backup_counts(const std::string& dir, Counts& counts)
{
    FILE* f = fopen(path_in(dir, "settings.dat").c_str(), "r");
    if (!f) {
        return false;
    }
    std::string line;
    size_t header = 0;
    bool ok = read_line(f, line) && (header = header_length(line)) > 0 &&
              sscanf(line.c_str() + header, "%d %d %d %d", &counts.profiles, &counts.networks,
                     &counts.hosts, &counts.history) == 4;
    fclose(f);
    return ok;
}

static bool parse_settings(const std::string& path, std::vector<Record>& records)
{
    FILE* f = fopen(path.c_str(), "r");
    if (!f) {
        return false;
    }
    std::string line;
    bool ok = read_line(f, line) && header_length(line) > 0;

    while (ok && read_line(f, line)) {
        if (records.size() >= MAX_RECORDS) {
            ok = false;
            break;
        }
        size_t a = line.find(' ');
        size_t b = a == std::string::npos ? a : line.find(' ', a + 1);
        size_t c = b == std::string::npos ? b : line.find(' ', b + 1);
        if (c == std::string::npos || c != b + 2) {
            ok = false;
            break;
        }
        Record r;
        r.ns = line.substr(0, a);
        r.key = line.substr(a + 1, b - a - 1);
        r.type = line[b + 1];
        std::string value = line.substr(c + 1);

        if (!known_namespace(r.ns) || !valid_key(r.key)) {
            ok = false;
        } else if (r.type == 's') {
            ok = from_hex(value, r.value) && r.value.size() <= 4000;
        } else if (r.type == 'b') {
            ok = from_hex(value, r.value, true) && !r.value.empty() && r.value.size() <= MAX_BLOB;
        } else if (r.type == 'u') {
            ok = !value.empty() && value.size() <= 10 &&
                 value.find_first_not_of("0123456789") == std::string::npos;
            r.value = value;
        } else {
            ok = false;
        }
        if (ok) {
            records.push_back(r);
        }
    }
    // A line that was too long or unterminated ends read_line early; treat as corrupt
    ok = ok && feof(f);
    fclose(f);
    return ok;
}

static void erase_namespaces()
{
    for (const char* ns : NAMESPACES) {
        nvs_handle_t handle;
        if (settings_nvs::open(ns, NVS_READWRITE, &handle) == ESP_OK) {
            nvs_erase_all(handle);
            nvs_commit(handle);
            nvs_close(handle);
        }
    }
}

esp_err_t settings_backup::restore(const std::string& dir, Counts& counts)
{
    std::vector<Record> records;
    if (!parse_settings(path_in(dir, "settings.dat"), records)) {
        ESP_LOGE(TAG, "Backup is missing or damaged");
        return ESP_ERR_INVALID_STATE;
    }

    erase_namespaces();
    esp_err_t result = ESP_OK;
    for (const char* ns : NAMESPACES) {
        nvs_handle_t handle;
        if (settings_nvs::open(ns, NVS_READWRITE, &handle) != ESP_OK) {
            result = ESP_FAIL;
            continue;
        }
        for (const auto& r : records) {
            if (r.ns != ns) {
                continue;
            }
            esp_err_t err = r.type == 's' ? nvs_set_str(handle, r.key.c_str(), r.value.c_str())
                          : r.type == 'b' ? nvs_set_blob(handle, r.key.c_str(), r.value.data(), r.value.size())
                                          : nvs_set_u32(handle, r.key.c_str(), strtoul(r.value.c_str(), NULL, 10));
            if (err != ESP_OK) {
                result = err;
            }
        }
        nvs_commit(handle);
        nvs_close(handle);
    }

    counts = count_records(records);
    return result;   // The caller re-opens the vault (vault::init) to use the restored passwords
}

esp_err_t settings_backup::erase_device()
{
    erase_namespaces();
    return vault::reset();
}
