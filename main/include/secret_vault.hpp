/*
 * Secret Vault
 * Encrypted storage for saved passwords (WiFi passwords, SSH passwords and SSH
 * key passphrases), kept in NVS alongside the other settings.
 *
 * A random 256-bit data key encrypts every secret with AES-256-GCM. The data key
 * is stored wrapped (AES-256-GCM) under either
 *   - a random per-device secret (default): unlocks automatically at boot, so
 *     saved WiFi reconnects with no prompts; or
 *   - a key derived from an optional master PIN (PBKDF2-HMAC-SHA256): the PIN is
 *     asked once per boot and never stored.
 * The device secret lives in its own NVS namespace that is never included in SD
 * card backups, so a backup's passwords can't be decrypted on another device
 * unless a PIN is set.
 */

#ifndef SECRET_VAULT_HPP
#define SECRET_VAULT_HPP

#include "esp_err.h"
#include <string>

#define VAULT_MIN_PIN_LENGTH 4

namespace vault
{
    // Opens the vault, creating it on first use and unlocking it automatically
    // when no PIN is set. Returns true if it had to discard passwords it couldn't
    // decrypt (e.g. a backup restored from another device without a PIN).
    bool init();

    bool is_available();    // NVS usable
    bool has_pin();         // A master PIN protects the vault
    bool is_unlocked();

    esp_err_t unlock(const std::string& pin);   // ESP_ERR_INVALID_ARG for a wrong PIN
    void lock();                                 // Only meaningful with a PIN
    esp_err_t set_pin(const std::string& pin);   // Vault must be unlocked
    esp_err_t change_pin(const std::string& old_pin, const std::string& new_pin);
    esp_err_t remove_pin(const std::string& pin);
    esp_err_t reset();                            // Delete every saved password; no PIN

    // Secrets are addressed by id, e.g. "wifi:HomeNet" or "profile:1a2b3c4d"
    esp_err_t put(const std::string& id, const std::string& secret);
    esp_err_t get(const std::string& id, std::string& secret);
    esp_err_t remove(const std::string& id);
    bool has(const std::string& id);
    int count();

    // Best-effort overwrite of a plaintext secret before it is released
    void wipe(std::string& s);
}

#endif
