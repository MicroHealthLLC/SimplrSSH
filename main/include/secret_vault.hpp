/*
 * Secret Vault
 * Encrypted storage for saved passwords (SSH passwords, SSH key passphrases and
 * WiFi passwords) as files on the internal "storage" SPIFFS partition.
 *
 * A random 256-bit data key encrypts every secret with AES-256-GCM. The data key
 * itself is stored wrapped (AES-256-GCM) under a key derived from the user's
 * master PIN with PBKDF2-HMAC-SHA256, so the PIN is never stored and changing it
 * only rewrites the small key file.
 */

#ifndef SECRET_VAULT_HPP
#define SECRET_VAULT_HPP

#include "esp_err.h"
#include <string>

#define VAULT_MIN_PIN_LENGTH 4

namespace vault
{
    // Mounts the storage partition (formatting it if it has never been used)
    esp_err_t init(const char* base_path = "/spiffs", const char* partition_label = "storage");

    bool is_mounted();
    bool is_set_up();       // A master PIN has been created
    bool is_unlocked();     // The data key is in memory

    esp_err_t setup(const std::string& pin);   // Create the vault; leaves it unlocked
    esp_err_t unlock(const std::string& pin);  // ESP_ERR_INVALID_ARG for a wrong PIN
    void lock();
    esp_err_t change_pin(const std::string& old_pin, const std::string& new_pin);
    esp_err_t reset();                          // Delete every secret and the PIN

    // Secrets are addressed by id, e.g. "wifi:HomeNet" or "profile:1a2b3c4d"
    esp_err_t put(const std::string& id, const std::string& secret);
    esp_err_t get(const std::string& id, std::string& secret);
    esp_err_t remove(const std::string& id);
    bool has(const std::string& id);

    // Best-effort overwrite of a plaintext secret before it is released
    void wipe(std::string& s);
}

#endif
