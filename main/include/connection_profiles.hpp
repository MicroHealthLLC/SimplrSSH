/*
 * Connection Profiles
 * Saved SSH connection profiles and saved WiFi networks, persisted to NVS so
 * they survive reboots. Passwords and key passphrases are never stored here;
 * they live encrypted in the secret vault (see secret_vault.hpp), keyed by
 * ConnectionProfile::secret_id() / SavedNetwork::secret_id().
 */

#ifndef CONNECTION_PROFILES_HPP
#define CONNECTION_PROFILES_HPP

#include "esp_err.h"
#include <string>
#include <vector>

#define PROFILE_MAX_COUNT 20
#define NETWORK_MAX_COUNT 10

struct ConnectionProfile
{
    enum class Auth { Password, Key };

    std::string id;          // Stable random id, so renaming keeps the saved secret
    std::string name;
    std::string host;
    int port = 22;
    std::string username;
    Auth auth = Auth::Password;
    std::string key_name;    // Key filename as loaded from /sdcard/ssh_keys/
    bool secret_saved = false;  // Password (or key passphrase) is in the vault

    std::string secret_id() const { return "profile:" + id; }
};

struct SavedNetwork
{
    std::string ssid;
    bool secret_saved = false;  // WiFi password is in the vault (false = open network)

    std::string secret_id() const { return "wifi:" + ssid; }
};

namespace profile_store
{
    std::vector<ConnectionProfile> load();
    esp_err_t save(const std::vector<ConnectionProfile>& profiles);
    std::string new_id();
}

namespace network_store
{
    std::vector<SavedNetwork> load();
    esp_err_t save(const std::vector<SavedNetwork>& networks);
}

#endif
