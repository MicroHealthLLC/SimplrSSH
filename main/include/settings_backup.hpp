/*
 * Settings Backup
 * Copies the device's saved settings to/from a folder (the SD card):
 *   settings.dat  NVS records of profiles, WiFi networks, trusted server keys and
 *                 command history (no secrets), hex encoded, one per line
 *   vault.dat     the encrypted vault files (passwords stay PIN-protected)
 * Restores validate the whole backup before changing anything on the device.
 */

#ifndef SETTINGS_BACKUP_HPP
#define SETTINGS_BACKUP_HPP

#include "esp_err.h"
#include <string>

namespace settings_backup
{
    struct Counts {
        int profiles = 0;
        int networks = 0;
        int hosts = 0;
        int history = 0;
        int vault_files = 0;
    };

    Counts count_device();                               // What is saved on this device
    bool read_backup_counts(const std::string& dir, Counts& counts);  // What a backup holds
    esp_err_t backup(const std::string& dir, Counts& counts);
    esp_err_t restore(const std::string& dir, Counts& counts);
    esp_err_t erase_device();                            // All settings and the vault
}

#endif
