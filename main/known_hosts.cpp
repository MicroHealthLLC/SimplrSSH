/*
 * Known Hosts
 * SSH host key verification, trust on first use: the first time a server is
 * seen its key fingerprint is shown and the user decides whether to trust it;
 * afterwards a changed key is refused (possible man-in-the-middle).
 *
 * Stored in the NVS namespace "known_hosts", one string per server:
 *   key   "h" + 14 hex chars of SHA-256("host:port")
 *   value "host:port" 0x1F key-type 0x1F "SHA256:<base64>"
 */

#include "ssh_terminal.hpp"
#include "esp_log.h"
#include "settings_nvs.hpp"
#include "mbedtls/base64.h"
#include "mbedtls/md.h"
#include <cstdio>
#include <cstdlib>
#include <algorithm>

static const char *TAG = "KNOWN_HOSTS";
static const char *NVS_NAMESPACE = "known_hosts";
static const char FIELD_SEP = '\x1F';

struct KnownHost {
    std::string nvs_key;
    std::string host_port;
    std::string key_type;
    std::string fingerprint;
};

static std::string host_port_of(const std::string& host, int port)
{
    return host + ":" + std::to_string(port);
}

static std::string nvs_key_for(const std::string& host_port)
{
    uint8_t hash[32];
    mbedtls_md(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), (const uint8_t*)host_port.data(), host_port.size(), hash);
    char key[16];
    snprintf(key, sizeof(key), "h%02x%02x%02x%02x%02x%02x%02x",
             hash[0], hash[1], hash[2], hash[3], hash[4], hash[5], hash[6]);
    return key;
}

static bool parse_record(const std::string& nvs_key, const std::string& value, KnownHost& out)
{
    size_t a = value.find(FIELD_SEP);
    size_t b = a == std::string::npos ? std::string::npos : value.find(FIELD_SEP, a + 1);
    if (b == std::string::npos) {
        return false;
    }
    out.nvs_key = nvs_key;
    out.host_port = value.substr(0, a);
    out.key_type = value.substr(a + 1, b - a - 1);
    out.fingerprint = value.substr(b + 1);
    return true;
}

static std::vector<KnownHost> load_known_hosts()
{
    std::vector<KnownHost> hosts;
    nvs_handle_t handle;
    if (settings_nvs::open(NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) {
        return hosts;
    }

    nvs_iterator_t it = NULL;
    esp_err_t err = nvs_entry_find(settings_nvs::partition(), NVS_NAMESPACE, NVS_TYPE_STR, &it);
    while (err == ESP_OK) {
        nvs_entry_info_t info;
        nvs_entry_info(it, &info);
        char value[160];
        size_t len = sizeof(value);
        KnownHost h;
        if (nvs_get_str(handle, info.key, value, &len) == ESP_OK && parse_record(info.key, value, h)) {
            hosts.push_back(h);
        }
        err = nvs_entry_next(&it);
    }
    nvs_release_iterator(it);
    nvs_close(handle);

    std::sort(hosts.begin(), hosts.end(),
              [](const KnownHost& a, const KnownHost& b) { return a.host_port < b.host_port; });
    return hosts;
}

static const char* key_type_name(int type)
{
    switch (type) {
        case LIBSSH2_HOSTKEY_TYPE_RSA: return "RSA";
        case LIBSSH2_HOSTKEY_TYPE_DSS: return "DSA";
        case LIBSSH2_HOSTKEY_TYPE_ECDSA_256: return "ECDSA-256";
        case LIBSSH2_HOSTKEY_TYPE_ECDSA_384: return "ECDSA-384";
        case LIBSSH2_HOSTKEY_TYPE_ECDSA_521: return "ECDSA-521";
        case LIBSSH2_HOSTKEY_TYPE_ED25519: return "ED25519";
        default: return "UNKNOWN";
    }
}

SSHTerminal::HostKeyStatus SSHTerminal::check_host_key(const std::string& host, int port,
                                                       std::string& fingerprint, std::string& key_type)
{
    size_t key_len = 0;
    int type = 0;
    const char* key = libssh2_session_hostkey(session, &key_len, &type);
    const char* hash = libssh2_hostkey_hash(session, LIBSSH2_HOSTKEY_HASH_SHA256);
    if (!key || !hash) {
        return HostKeyStatus::Error;
    }

    // Same format as OpenSSH: "SHA256:" + unpadded base64 of the 32-byte hash
    unsigned char b64[64];
    size_t b64_len = 0;
    if (mbedtls_base64_encode(b64, sizeof(b64), &b64_len, (const unsigned char*)hash, 32) != 0) {
        return HostKeyStatus::Error;
    }
    while (b64_len > 0 && b64[b64_len - 1] == '=') {
        b64_len--;
    }
    fingerprint = "SHA256:" + std::string((const char*)b64, b64_len);
    key_type = key_type_name(type);

    std::string host_port = host_port_of(host, port);
    nvs_handle_t handle;
    if (settings_nvs::open(NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) {
        return HostKeyStatus::Unknown;
    }
    char value[160];
    size_t len = sizeof(value);
    std::string nvs_key = nvs_key_for(host_port);
    esp_err_t err = nvs_get_str(handle, nvs_key.c_str(), value, &len);
    nvs_close(handle);

    KnownHost known;
    if (err != ESP_OK || !parse_record(nvs_key, value, known) || known.host_port != host_port) {
        return HostKeyStatus::Unknown;
    }
    if (known.fingerprint == fingerprint && known.key_type == key_type) {
        return HostKeyStatus::Match;
    }
    ESP_LOGW(TAG, "Host key changed for %s", host_port.c_str());
    return HostKeyStatus::Mismatch;
}

esp_err_t SSHTerminal::save_host_key(const std::string& host, int port, const std::string& key_type,
                                     const std::string& fingerprint)
{
    std::string host_port = host_port_of(host, port);
    std::string value = host_port + FIELD_SEP + key_type + FIELD_SEP + fingerprint;

    nvs_handle_t handle;
    esp_err_t err = settings_nvs::open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err == ESP_OK) {
        err = nvs_set_str(handle, nvs_key_for(host_port).c_str(), value.c_str());
        if (err == ESP_OK) {
            err = nvs_commit(handle);
        }
        nvs_close(handle);
    }
    return err;
}

void SSHTerminal::handle_hosts_command(const std::string& command)
{
    size_t space = command.find(' ');
    std::string rest = space == std::string::npos ? "" : command.substr(space + 1);
    rest.erase(0, rest.find_first_not_of(' '));
    size_t sub_end = rest.find(' ');
    std::string sub = rest.substr(0, sub_end);
    std::string ref = sub_end == std::string::npos ? "" : rest.substr(sub_end + 1);
    ref.erase(0, ref.find_first_not_of(' '));
    while (!ref.empty() && ref.back() == ' ') {
        ref.pop_back();
    }

    wizard_reset();
    std::vector<KnownHost> hosts = load_known_hosts();

    if (sub.empty() || sub == "list" || sub == "ls") {
        if (hosts.empty()) {
            append_text("No trusted servers yet. A server's key is saved the first time you trust it.\n");
            return;
        }
        append_text("Trusted server keys:\n");
        for (size_t i = 0; i < hosts.size(); i++) {
            append_text((" " + std::to_string(i + 1) + ") " + hosts[i].host_port + "  " + hosts[i].key_type +
                         "\n    " + hosts[i].fingerprint + "\n").c_str());
        }
        return;
    }

    if (sub == "forget" || sub == "rm") {
        int index = -1;
        if (!ref.empty() && std::all_of(ref.begin(), ref.end(), ::isdigit)) {
            int n = std::atoi(ref.c_str());
            index = (n >= 1 && n <= (int)hosts.size()) ? n - 1 : -1;
        } else if (!ref.empty()) {
            std::string wanted = ref.find(':') == std::string::npos ? ref + ":22" : ref;
            for (size_t i = 0; i < hosts.size(); i++) {
                if (hosts[i].host_port == wanted) {
                    index = i;
                }
            }
        }
        if (index < 0) {
            append_text("Usage: hosts forget <HOST[:PORT]|#>  (see 'hosts' for the list)\n");
            return;
        }
        forget_host = hosts[index].host_port;
        wizard_goto(WizardStep::HostConfirmForget);
        return;
    }

    append_text("Usage: hosts [list] | hosts forget <HOST[:PORT]|#>\n");
}

bool SSHTerminal::host_step_prompt()
{
    switch (wizard.step) {
        case WizardStep::HostTrust:
            append_text("Trust this server and continue? (y/n) [n]: ");
            wizard.choices = {"n", "y"};
            return true;
        case WizardStep::HostConfirmForget:
            append_text(("Forget the saved key for " + forget_host + "? (y/n) [n]: ").c_str());
            wizard.choices = {"n", "y"};
            return true;
        default:
            return false;
    }
}

bool SSHTerminal::host_step_input(const std::string& raw_input, const std::string& input)
{
    (void)raw_input;
    std::string answer = input;
    std::transform(answer.begin(), answer.end(), answer.begin(), ::tolower);
    bool yes = answer == "y" || answer == "yes";

    switch (wizard.step) {
        case WizardStep::HostTrust:
            wizard_reset();
            if (!yes) {
                append_text("Not trusted - disconnected.\n");
                ssh_teardown();
                go_home();
            } else {
                if (save_host_key(pending_ssh.host, pending_ssh.port, pending_ssh.key_type,
                                  pending_ssh.fingerprint) == ESP_OK) {
                    append_text("Server key saved.\n");
                } else {
                    append_text("WARNING: Could not save the server key; you'll be asked again.\n");
                }
                ssh_finish();
            }
            return true;

        case WizardStep::HostConfirmForget: {
            std::string host_port = forget_host;
            forget_host.clear();
            wizard_reset();
            if (yes) {
                nvs_handle_t handle;
                if (settings_nvs::open(NVS_NAMESPACE, NVS_READWRITE, &handle) == ESP_OK) {
                    nvs_erase_key(handle, nvs_key_for(host_port).c_str());
                    nvs_commit(handle);
                    nvs_close(handle);
                }
                append_text(("Forgot " + host_port + ". Its key will be shown on the next connect.\n").c_str());
            } else {
                append_text("Kept.\n");
            }
            return true;
        }

        default:
            return false;
    }
}
