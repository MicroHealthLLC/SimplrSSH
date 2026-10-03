/*
 * In-memory implementations of the ESP-IDF calls the host-tested firmware sources make.
 */

#include "fake_idf.hpp"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_random.h"
#include "nvs_flash.h"
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <deque>
#include <random>
#include <set>

namespace
{
    struct Handle {
        std::string part;
        std::string ns;
        bool writable;
    };

    struct PartitionState {
        esp_partition_t info;
        esp_err_t first_init = ESP_OK;
        bool init_attempted = false;
        int erases = 0;
    };

    fake::Store s_store;
    std::set<std::string> s_ready;                 // Initialized partitions
    std::map<nvs_handle_t, Handle> s_handles;
    nvs_handle_t s_next_handle = 1;
    std::deque<PartitionState> s_table;            // deque: pointers stay valid
    const esp_partition_t* s_running = nullptr;
    bool s_fail_writes = false;
    std::vector<std::string> s_log;
    std::mt19937 s_rng;

    const size_t MAX_STR = 4000;                   // NVS string limit incl. NUL
    const size_t MAX_BLOB = 508000;

    bool valid_name(const char* name)
    {
        return name && name[0] && strlen(name) < NVS_KEY_NAME_MAX_SIZE;
    }

    Handle* handle_of(nvs_handle_t h)
    {
        auto it = s_handles.find(h);
        return it == s_handles.end() ? nullptr : &it->second;
    }

    esp_err_t write_value(nvs_handle_t h, const char* key, nvs_type_t type, const std::string& bytes)
    {
        Handle* hd = handle_of(h);
        if (!hd) {
            return ESP_ERR_NVS_INVALID_HANDLE;
        }
        if (!hd->writable) {
            return ESP_ERR_NVS_READ_ONLY;
        }
        if (!key || !key[0]) {
            return ESP_ERR_NVS_INVALID_NAME;
        }
        if (strlen(key) >= NVS_KEY_NAME_MAX_SIZE) {
            return ESP_ERR_NVS_KEY_TOO_LONG;
        }
        if (s_fail_writes) {
            return ESP_ERR_NVS_NOT_ENOUGH_SPACE;
        }
        s_store[hd->part][hd->ns][key] = fake::Value{type, bytes};
        return ESP_OK;
    }

    esp_err_t read_value(nvs_handle_t h, const char* key, nvs_type_t type, const fake::Value** out)
    {
        Handle* hd = handle_of(h);
        if (!hd) {
            return ESP_ERR_NVS_INVALID_HANDLE;
        }
        if (!key || strlen(key) >= NVS_KEY_NAME_MAX_SIZE) {
            return ESP_ERR_NVS_KEY_TOO_LONG;
        }
        auto& keys = s_store[hd->part][hd->ns];
        auto it = keys.find(key);
        if (it == keys.end() || it->second.type != type) {
            return ESP_ERR_NVS_NOT_FOUND;
        }
        *out = &it->second;
        return ESP_OK;
    }

    PartitionState* state_of(const esp_partition_t* p)
    {
        for (auto& s : s_table) {
            if (&s.info == p) {
                return &s;
            }
        }
        return nullptr;
    }
}

struct nvs_opaque_iterator_t {
    std::vector<nvs_entry_info_t> entries;
    size_t pos = 0;
};

// ---------------------------------------------------------------------------
// Test controls
// ---------------------------------------------------------------------------

void fake::reset()
{
    s_store.clear();
    s_ready.clear();
    s_handles.clear();
    s_next_handle = 1;
    s_table.clear();
    s_running = nullptr;
    s_fail_writes = false;
    s_log.clear();
    s_rng.seed(12345);
}

void fake::seed(uint32_t value)
{
    s_rng.seed(value);
}

fake::Store& fake::store()
{
    return s_store;
}

void fake::init_partition(const char* label)
{
    s_ready.insert(label);
}

std::string fake::dump_all()
{
    std::string all;
    for (const auto& part : s_store) {
        for (const auto& ns : part.second) {
            for (const auto& kv : ns.second) {
                all += part.first + "/" + ns.first + "/" + kv.first + "=" + kv.second.bytes + "\n";
            }
        }
    }
    return all;
}

int fake::open_handles()
{
    return (int)s_handles.size();
}

void fake::add_partition(esp_partition_type_t type, esp_partition_subtype_t subtype, uint32_t size,
                         const char* label)
{
    PartitionState s;
    memset(&s.info, 0, sizeof(s.info));
    s.info.type = type;
    s.info.subtype = subtype;
    s.info.size = size;
    snprintf(s.info.label, sizeof(s.info.label), "%s", label);
    s_table.push_back(s);
}

void fake::set_running(esp_partition_subtype_t app_subtype)
{
    add_partition(ESP_PARTITION_TYPE_APP, app_subtype, 0x900000, "app");
    s_running = &s_table.back().info;
}

void fake::set_init_result(const char* label, esp_err_t first_result)
{
    for (auto& s : s_table) {
        if (strcmp(s.info.label, label) == 0) {
            s.first_init = first_result;
        }
    }
}

int fake::erase_count(const char* label)
{
    for (auto& s : s_table) {
        if (strcmp(s.info.label, label) == 0) {
            return s.erases;
        }
    }
    return 0;
}

void fake::fail_writes(bool fail)
{
    s_fail_writes = fail;
}

const std::vector<std::string>& fake::log()
{
    return s_log;
}

// ---------------------------------------------------------------------------
// esp_err / esp_log / esp_random
// ---------------------------------------------------------------------------

const char* esp_err_to_name(esp_err_t code)
{
    switch (code) {
        case ESP_OK: return "ESP_OK";
        case ESP_FAIL: return "ESP_FAIL";
        case ESP_ERR_NVS_NOT_FOUND: return "ESP_ERR_NVS_NOT_FOUND";
        case ESP_ERR_NVS_NO_FREE_PAGES: return "ESP_ERR_NVS_NO_FREE_PAGES";
        case ESP_ERR_NVS_NOT_ENOUGH_SPACE: return "ESP_ERR_NVS_NOT_ENOUGH_SPACE";
        default: return "ESP_ERR_OTHER";
    }
}

void fake_log(char level, const char* tag, const char* fmt, ...)
{
    char buf[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    s_log.push_back(std::string(1, level) + " " + tag + " " + buf);
}

uint32_t esp_random(void)
{
    return s_rng();
}

void esp_fill_random(void* buf, size_t len)
{
    uint8_t* p = (uint8_t*)buf;
    for (size_t i = 0; i < len; i++) {
        p[i] = (uint8_t)s_rng();
    }
}

// ---------------------------------------------------------------------------
// Partitions
// ---------------------------------------------------------------------------

const esp_partition_t* esp_partition_find_first(esp_partition_type_t type, esp_partition_subtype_t subtype,
                                                const char* label)
{
    for (auto& s : s_table) {
        if (s.info.type == type && (subtype == ESP_PARTITION_SUBTYPE_ANY || s.info.subtype == subtype) &&
            (!label || strcmp(s.info.label, label) == 0)) {
            return &s.info;
        }
    }
    return nullptr;
}

const esp_partition_t* esp_ota_get_running_partition(void)
{
    return s_running;
}

esp_err_t nvs_flash_init_partition_ptr(const esp_partition_t* partition)
{
    PartitionState* s = state_of(partition);
    if (!s) {
        return ESP_ERR_NOT_FOUND;
    }
    if (!s->init_attempted) {
        s->init_attempted = true;
        if (s->first_init != ESP_OK) {
            return s->first_init;
        }
    }
    s_ready.insert(partition->label);
    return ESP_OK;
}

esp_err_t nvs_flash_erase_partition_ptr(const esp_partition_t* partition)
{
    PartitionState* s = state_of(partition);
    if (!s) {
        return ESP_ERR_NOT_FOUND;
    }
    s->erases++;
    s_store.erase(partition->label);
    return ESP_OK;
}

// ---------------------------------------------------------------------------
// NVS
// ---------------------------------------------------------------------------

esp_err_t nvs_open_from_partition(const char* part_name, const char* ns, nvs_open_mode_t mode, nvs_handle_t* out)
{
    if (!s_ready.count(part_name)) {
        return ESP_ERR_NVS_NOT_INITIALIZED;
    }
    if (!valid_name(ns)) {
        return ESP_ERR_NVS_INVALID_NAME;
    }
    auto& part = s_store[part_name];
    if (mode == NVS_READONLY && !part.count(ns)) {
        return ESP_ERR_NVS_NOT_FOUND;
    }
    part[ns];
    *out = s_next_handle++;
    s_handles[*out] = Handle{part_name, ns, mode == NVS_READWRITE};
    return ESP_OK;
}

void nvs_close(nvs_handle_t handle)
{
    s_handles.erase(handle);
}

esp_err_t nvs_commit(nvs_handle_t handle)
{
    return handle_of(handle) ? ESP_OK : ESP_ERR_NVS_INVALID_HANDLE;
}

esp_err_t nvs_set_str(nvs_handle_t handle, const char* key, const char* value)
{
    if (strlen(value) + 1 > MAX_STR) {
        return ESP_ERR_NVS_VALUE_TOO_LONG;
    }
    return write_value(handle, key, NVS_TYPE_STR, value);
}

esp_err_t nvs_get_str(nvs_handle_t handle, const char* key, char* out, size_t* length)
{
    const fake::Value* v;
    esp_err_t err = read_value(handle, key, NVS_TYPE_STR, &v);
    if (err != ESP_OK) {
        return err;
    }
    size_t needed = v->bytes.size() + 1;
    if (!out) {
        *length = needed;
        return ESP_OK;
    }
    if (*length < needed) {
        return ESP_ERR_NVS_INVALID_LENGTH;
    }
    memcpy(out, v->bytes.c_str(), needed);
    *length = needed;
    return ESP_OK;
}

esp_err_t nvs_set_blob(nvs_handle_t handle, const char* key, const void* value, size_t length)
{
    if (length > MAX_BLOB) {
        return ESP_ERR_NVS_VALUE_TOO_LONG;
    }
    return write_value(handle, key, NVS_TYPE_BLOB, std::string((const char*)value, length));
}

esp_err_t nvs_get_blob(nvs_handle_t handle, const char* key, void* out, size_t* length)
{
    const fake::Value* v;
    esp_err_t err = read_value(handle, key, NVS_TYPE_BLOB, &v);
    if (err != ESP_OK) {
        return err;
    }
    if (!out) {
        *length = v->bytes.size();
        return ESP_OK;
    }
    if (*length < v->bytes.size()) {
        return ESP_ERR_NVS_INVALID_LENGTH;
    }
    memcpy(out, v->bytes.data(), v->bytes.size());
    *length = v->bytes.size();
    return ESP_OK;
}

esp_err_t nvs_set_u32(nvs_handle_t handle, const char* key, uint32_t value)
{
    return write_value(handle, key, NVS_TYPE_U32, std::string((const char*)&value, sizeof(value)));
}

esp_err_t nvs_get_u32(nvs_handle_t handle, const char* key, uint32_t* out)
{
    const fake::Value* v;
    esp_err_t err = read_value(handle, key, NVS_TYPE_U32, &v);
    if (err == ESP_OK) {
        memcpy(out, v->bytes.data(), sizeof(*out));
    }
    return err;
}

esp_err_t nvs_erase_key(nvs_handle_t handle, const char* key)
{
    Handle* hd = handle_of(handle);
    if (!hd) {
        return ESP_ERR_NVS_INVALID_HANDLE;
    }
    if (!hd->writable) {
        return ESP_ERR_NVS_READ_ONLY;
    }
    if (s_fail_writes) {
        return ESP_ERR_NVS_NOT_ENOUGH_SPACE;
    }
    return s_store[hd->part][hd->ns].erase(key) ? ESP_OK : ESP_ERR_NVS_NOT_FOUND;
}

esp_err_t nvs_erase_all(nvs_handle_t handle)
{
    Handle* hd = handle_of(handle);
    if (!hd) {
        return ESP_ERR_NVS_INVALID_HANDLE;
    }
    if (!hd->writable) {
        return ESP_ERR_NVS_READ_ONLY;
    }
    s_store[hd->part][hd->ns].clear();
    return ESP_OK;
}

esp_err_t nvs_entry_find(const char* part_name, const char* ns, nvs_type_t type, nvs_iterator_t* it)
{
    *it = nullptr;
    if (!s_ready.count(part_name)) {
        return ESP_ERR_NVS_NOT_INITIALIZED;
    }
    auto* iter = new nvs_opaque_iterator_t;
    for (const auto& n : s_store[part_name]) {
        if (ns && n.first != ns) {
            continue;
        }
        for (const auto& kv : n.second) {
            if (type != NVS_TYPE_ANY && kv.second.type != type) {
                continue;
            }
            nvs_entry_info_t info = {};
            snprintf(info.namespace_name, sizeof(info.namespace_name), "%s", n.first.c_str());
            snprintf(info.key, sizeof(info.key), "%s", kv.first.c_str());
            info.type = kv.second.type;
            iter->entries.push_back(info);
        }
    }
    if (iter->entries.empty()) {
        delete iter;
        return ESP_ERR_NVS_NOT_FOUND;
    }
    *it = iter;
    return ESP_OK;
}

esp_err_t nvs_entry_next(nvs_iterator_t* it)
{
    if (!it || !*it) {
        return ESP_ERR_INVALID_ARG;
    }
    if (++(*it)->pos >= (*it)->entries.size()) {
        delete *it;
        *it = nullptr;
        return ESP_ERR_NVS_NOT_FOUND;
    }
    return ESP_OK;
}

esp_err_t nvs_entry_info(nvs_iterator_t it, nvs_entry_info_t* info)
{
    if (!it) {
        return ESP_ERR_INVALID_ARG;
    }
    *info = it->entries[it->pos];
    return ESP_OK;
}

void nvs_release_iterator(nvs_iterator_t it)
{
    delete it;
}
