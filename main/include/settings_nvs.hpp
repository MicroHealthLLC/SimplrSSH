/*
 * Settings NVS
 * The NVS partition that holds every saved setting (profiles, networks, known hosts,
 * history, vault). Flashed directly (merged image at 0x0) it is "nvs" after the app.
 * Installed by a launcher (e.g. bmorcelli/Launcher) the device's "nvs" belongs to the
 * launcher, so PocketSSH uses its own "pocketssh" data partition, which the launcher
 * creates from partitions_launcher.csv and keeps across updates.
 * All settings code opens namespaces through settings_nvs::open().
 */

#ifndef SETTINGS_NVS_HPP
#define SETTINGS_NVS_HPP

#include "esp_err.h"
#include "nvs.h"

namespace settings_nvs
{
    enum class InitResult {
        OK,            // Settings partition ready
        REFORMATTED,   // Was unreadable and has been reset (own partition only)
        SHARED,        // Running from a launcher without the "pocketssh" partition: using the
                       // launcher's small "nvs", which is never reformatted
        UNAVAILABLE,   // No usable settings partition: nothing can be saved
    };

    InitResult init();                     // Call once at boot, before anything reads settings
    const char* partition();               // Label for nvs_entry_find() and friends
    esp_err_t open(const char* ns, nvs_open_mode_t mode, nvs_handle_t* handle);
}

#endif
