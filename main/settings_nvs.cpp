/*
 * Settings NVS Implementation
 * Picks the partition that holds the saved settings (see settings_nvs.hpp):
 *   "simplrssh" (any data subtype)  installed by a launcher from the -launcher.bin image;
 *                                   ours, so reformatted if unreadable
 *   "pocketssh" (any data subtype)  same, created by an image from before the rename to
 *                                   SimplrSSH; still used so those settings are kept
 *   "nvs", app in "factory"         flashed directly at 0x0; ours, reformatted if unreadable
 *   "nvs", app elsewhere (ota_N)    a launcher's own settings; used as-is, never erased
 */

#include "settings_nvs.hpp"
#include "nvs_flash.h"
#include "esp_partition.h"
#include "esp_ota_ops.h"
#include "esp_log.h"
#include <initializer_list>

static const char *TAG = "SETTINGS";

static const char* const OWN_LABEL = "simplrssh";
static const char* const LEGACY_LABEL = "pocketssh";   // Label used before the rename
static const size_t MIN_SIZE = 0x4000;   // NVS needs a few 4 KB pages to be useful
static const char* active_label = NVS_DEFAULT_PART_NAME;

static bool unreadable(esp_err_t err)
{
    return err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND;
}

static settings_nvs::InitResult init_own(const esp_partition_t* part)
{
    esp_err_t err = nvs_flash_init_partition_ptr(part);
    if (err == ESP_OK) {
        return settings_nvs::InitResult::OK;
    }
    if (!unreadable(err) && part->subtype == ESP_PARTITION_SUBTYPE_DATA_NVS) {
        ESP_LOGE(TAG, "Settings partition failed (%s)", esp_err_to_name(err));
        return settings_nvs::InitResult::UNAVAILABLE;
    }
    // Unreadable, or a launcher left other data in the partition it created for us
    ESP_LOGE(TAG, "Settings unreadable (%s), reformatting", esp_err_to_name(err));
    if (nvs_flash_erase_partition_ptr(part) != ESP_OK || nvs_flash_init_partition_ptr(part) != ESP_OK) {
        ESP_LOGE(TAG, "Settings partition could not be reformatted");
        return settings_nvs::InitResult::UNAVAILABLE;
    }
    return settings_nvs::InitResult::REFORMATTED;
}

settings_nvs::InitResult settings_nvs::init()
{
    // The legacy partition is checked first: if a launcher kept it next to a new, empty one,
    // it still holds the user's settings
    for (const char* label : {LEGACY_LABEL, OWN_LABEL}) {
        const esp_partition_t* own = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, label);
        if (own && own->size >= MIN_SIZE) {
            active_label = label;
            return init_own(own);
        }
    }

    const esp_partition_t* nvs = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_NVS,
                                                          NVS_DEFAULT_PART_NAME);
    if (!nvs) {
        ESP_LOGE(TAG, "No settings partition");
        return InitResult::UNAVAILABLE;
    }
    active_label = NVS_DEFAULT_PART_NAME;

    const esp_partition_t* running = esp_ota_get_running_partition();
    if (running && running->subtype == ESP_PARTITION_SUBTYPE_APP_FACTORY) {
        return init_own(nvs);
    }

    // Installed by a launcher from an image without the "simplrssh" partition
    ESP_LOGW(TAG, "Using the launcher's nvs partition");
    esp_err_t err = nvs_flash_init_partition_ptr(nvs);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Launcher's nvs unusable (%s), not erasing it", esp_err_to_name(err));
        return InitResult::UNAVAILABLE;
    }
    return InitResult::SHARED;
}

const char* settings_nvs::partition()
{
    return active_label;
}

esp_err_t settings_nvs::open(const char* ns, nvs_open_mode_t mode, nvs_handle_t* handle)
{
    return nvs_open_from_partition(active_label, ns, mode, handle);
}
