// Host stand-in for ESP-IDF's nvs.h, backed by the in-memory fake in fake_idf.cpp.
// Mirrors the real API's behaviour where the firmware depends on it: 15-character keys,
// NOT_FOUND for missing namespaces opened read-only, length queries with a NULL buffer.
#pragma once
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#define NVS_DEFAULT_PART_NAME "nvs"
#define NVS_KEY_NAME_MAX_SIZE 16
#define NVS_NS_NAME_MAX_SIZE NVS_KEY_NAME_MAX_SIZE

typedef uint32_t nvs_handle_t;
typedef enum { NVS_READONLY, NVS_READWRITE } nvs_open_mode_t;
typedef enum {
    NVS_TYPE_U32 = 0x04,
    NVS_TYPE_STR = 0x21,
    NVS_TYPE_BLOB = 0x42,
    NVS_TYPE_ANY = 0xff,
} nvs_type_t;

typedef struct {
    char namespace_name[NVS_NS_NAME_MAX_SIZE];
    char key[NVS_KEY_NAME_MAX_SIZE];
    nvs_type_t type;
} nvs_entry_info_t;

typedef struct nvs_opaque_iterator_t* nvs_iterator_t;

esp_err_t nvs_open_from_partition(const char* part_name, const char* ns, nvs_open_mode_t mode, nvs_handle_t* out);
void nvs_close(nvs_handle_t handle);
esp_err_t nvs_commit(nvs_handle_t handle);
esp_err_t nvs_set_str(nvs_handle_t handle, const char* key, const char* value);
esp_err_t nvs_get_str(nvs_handle_t handle, const char* key, char* out, size_t* length);
esp_err_t nvs_set_blob(nvs_handle_t handle, const char* key, const void* value, size_t length);
esp_err_t nvs_get_blob(nvs_handle_t handle, const char* key, void* out, size_t* length);
esp_err_t nvs_set_u32(nvs_handle_t handle, const char* key, uint32_t value);
esp_err_t nvs_get_u32(nvs_handle_t handle, const char* key, uint32_t* out);
esp_err_t nvs_erase_key(nvs_handle_t handle, const char* key);
esp_err_t nvs_erase_all(nvs_handle_t handle);
esp_err_t nvs_entry_find(const char* part_name, const char* ns, nvs_type_t type, nvs_iterator_t* it);
esp_err_t nvs_entry_next(nvs_iterator_t* it);
esp_err_t nvs_entry_info(nvs_iterator_t it, nvs_entry_info_t* info);
void nvs_release_iterator(nvs_iterator_t it);
