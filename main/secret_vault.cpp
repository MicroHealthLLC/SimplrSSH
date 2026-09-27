/*
 * Secret Vault Implementation
 *
 * NVS namespace "vault" (included in SD card backups):
 *   "key"          magic[4] | iterations (u32 LE) | salt[16] | iv[12] | tag[16] | wrapped data key[32]
 *                  magic "PSKD": wrapped with the device secret; "PSK1": with the master PIN
 *   "s<14 hex>"    iv[12] | tag[16] | ciphertext  - one per secret; the name is a truncated
 *                  SHA-256 of the secret id and is bound to the ciphertext as GCM AAD
 * NVS namespace "vault_dev" (never backed up):
 *   "secret"       32 random bytes, created on first boot
 */

#include "secret_vault.hpp"
#include "esp_log.h"
#include "esp_random.h"
#include "nvs.h"
#include "mbedtls/gcm.h"
#include "mbedtls/md.h"
#include "mbedtls/platform_util.h"
#include <cstdio>
#include <cstring>
#include <vector>

static const char *TAG = "VAULT";

static const char *NS_VAULT = "vault";
static const char *NS_DEVICE = "vault_dev";
static const char MAGIC_DEVICE[4] = {'P', 'S', 'K', 'D'};
static const char MAGIC_PIN[4] = {'P', 'S', 'K', '1'};
static const uint32_t PIN_ITERATIONS = 20000;
static const uint32_t DEVICE_ITERATIONS = 1;   // The device secret is already a random 256-bit key
static const size_t SALT_LEN = 16;
static const size_t IV_LEN = 12;
static const size_t TAG_LEN = 16;
static const size_t KEY_LEN = 32;
static const size_t KEY_RECORD_LEN = 4 + 4 + SALT_LEN + IV_LEN + TAG_LEN + KEY_LEN;
static const size_t MAX_SECRET_LEN = 256;

static bool s_available = false;
static bool s_unlocked = false;
static bool s_has_pin = false;
static uint8_t s_data_key[KEY_LEN];

// ---------------------------------------------------------------------------
// NVS helpers
// ---------------------------------------------------------------------------

static bool nvs_read_blob(const char* ns, const char* key, std::vector<uint8_t>& out)
{
    nvs_handle_t h;
    if (nvs_open(ns, NVS_READONLY, &h) != ESP_OK) {
        return false;
    }
    size_t len = 0;
    bool ok = nvs_get_blob(h, key, NULL, &len) == ESP_OK && len > 0 && len <= 1024;
    if (ok) {
        out.resize(len);
        ok = nvs_get_blob(h, key, out.data(), &len) == ESP_OK;
    }
    nvs_close(h);
    return ok;
}

static esp_err_t nvs_write_blob(const char* ns, const char* key, const void* data, size_t len)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(ns, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = nvs_set_blob(h, key, data, len);
        if (err == ESP_OK) {
            err = nvs_commit(h);
        }
        nvs_close(h);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to save %s/%s: %s", ns, key, esp_err_to_name(err));
    }
    return err;
}

static void nvs_erase(const char* ns, const char* key)
{
    nvs_handle_t h;
    if (nvs_open(ns, NVS_READWRITE, &h) == ESP_OK) {
        if (key) {
            nvs_erase_key(h, key);
        } else {
            nvs_erase_all(h);
        }
        nvs_commit(h);
        nvs_close(h);
    }
}

// ---------------------------------------------------------------------------
// Crypto helpers
// ---------------------------------------------------------------------------

// PBKDF2-HMAC-SHA256 producing a single 32-byte block
static bool derive_key(const uint8_t* password, size_t password_len, const uint8_t* salt, uint32_t iterations,
                       uint8_t* out)
{
    const mbedtls_md_info_t* info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    mbedtls_md_context_t ctx;
    mbedtls_md_init(&ctx);

    uint8_t u[KEY_LEN];
    const uint8_t block_index[4] = {0, 0, 0, 1};
    bool ok = mbedtls_md_setup(&ctx, info, 1) == 0 &&
              mbedtls_md_hmac_starts(&ctx, password, password_len) == 0 &&
              mbedtls_md_hmac_update(&ctx, salt, SALT_LEN) == 0 &&
              mbedtls_md_hmac_update(&ctx, block_index, sizeof(block_index)) == 0 &&
              mbedtls_md_hmac_finish(&ctx, u) == 0;

    if (ok) {
        memcpy(out, u, KEY_LEN);
        for (uint32_t i = 1; i < iterations && ok; i++) {
            ok = mbedtls_md_hmac_reset(&ctx) == 0 &&
                 mbedtls_md_hmac_update(&ctx, u, KEY_LEN) == 0 &&
                 mbedtls_md_hmac_finish(&ctx, u) == 0;
            for (size_t j = 0; j < KEY_LEN; j++) {
                out[j] ^= u[j];
            }
        }
    }

    mbedtls_platform_zeroize(u, sizeof(u));
    mbedtls_md_free(&ctx);
    return ok;
}

static bool gcm_encrypt(const uint8_t* key, const uint8_t* iv, const std::string& aad,
                        const uint8_t* in, size_t len, uint8_t* out, uint8_t* tag)
{
    mbedtls_gcm_context gcm;
    mbedtls_gcm_init(&gcm);
    bool ok = mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, key, KEY_LEN * 8) == 0 &&
              mbedtls_gcm_crypt_and_tag(&gcm, MBEDTLS_GCM_ENCRYPT, len, iv, IV_LEN,
                                        (const uint8_t*)aad.data(), aad.size(),
                                        in, out, TAG_LEN, tag) == 0;
    mbedtls_gcm_free(&gcm);
    return ok;
}

static bool gcm_decrypt(const uint8_t* key, const uint8_t* iv, const std::string& aad,
                        const uint8_t* in, size_t len, const uint8_t* tag, uint8_t* out)
{
    mbedtls_gcm_context gcm;
    mbedtls_gcm_init(&gcm);
    bool ok = mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, key, KEY_LEN * 8) == 0 &&
              mbedtls_gcm_auth_decrypt(&gcm, len, iv, IV_LEN,
                                       (const uint8_t*)aad.data(), aad.size(),
                                       tag, TAG_LEN, in, out) == 0;
    mbedtls_gcm_free(&gcm);
    return ok;
}

static std::string secret_key_name(const std::string& id)
{
    uint8_t hash[32];
    mbedtls_md(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), (const uint8_t*)id.data(), id.size(), hash);
    char name[16];
    snprintf(name, sizeof(name), "s%02x%02x%02x%02x%02x%02x%02x",
             hash[0], hash[1], hash[2], hash[3], hash[4], hash[5], hash[6]);
    return name;
}

// The per-device secret used when no PIN is set (created on first use)
static bool device_secret(std::string& out)
{
    std::vector<uint8_t> secret;
    if (!nvs_read_blob(NS_DEVICE, "secret", secret) || secret.size() != KEY_LEN) {
        secret.resize(KEY_LEN);
        esp_fill_random(secret.data(), KEY_LEN);
        if (nvs_write_blob(NS_DEVICE, "secret", secret.data(), KEY_LEN) != ESP_OK) {
            return false;
        }
    }
    out.assign((const char*)secret.data(), KEY_LEN);
    mbedtls_platform_zeroize(secret.data(), secret.size());
    return true;
}

// Wrap the data key under the device secret (pin empty) or a PIN, and store it
static esp_err_t write_key_record(const std::string* pin, const uint8_t* data_key)
{
    std::string password;
    if (pin) {
        password = *pin;
    } else if (!device_secret(password)) {
        return ESP_FAIL;
    }
    const uint32_t iterations = pin ? PIN_ITERATIONS : DEVICE_ITERATIONS;

    uint8_t record[KEY_RECORD_LEN];
    memcpy(record, pin ? MAGIC_PIN : MAGIC_DEVICE, 4);
    record[4] = iterations & 0xFF;
    record[5] = (iterations >> 8) & 0xFF;
    record[6] = (iterations >> 16) & 0xFF;
    record[7] = (iterations >> 24) & 0xFF;
    uint8_t* salt = record + 8;
    uint8_t* iv = salt + SALT_LEN;
    uint8_t* tag = iv + IV_LEN;
    uint8_t* wrapped = tag + TAG_LEN;
    esp_fill_random(salt, SALT_LEN);
    esp_fill_random(iv, IV_LEN);

    uint8_t kek[KEY_LEN];
    bool ok = derive_key((const uint8_t*)password.data(), password.size(), salt, iterations, kek) &&
              gcm_encrypt(kek, iv, "key", data_key, KEY_LEN, wrapped, tag);
    mbedtls_platform_zeroize(kek, sizeof(kek));
    vault::wipe(password);
    if (!ok) {
        return ESP_FAIL;
    }
    // A single NVS write replaces the record atomically
    return nvs_write_blob(NS_VAULT, "key", record, sizeof(record));
}

// Unwrap the stored data key with the device secret (pin NULL) or a PIN
static esp_err_t read_key_record(const std::string* pin, uint8_t* data_key)
{
    std::vector<uint8_t> record;
    if (!nvs_read_blob(NS_VAULT, "key", record) || record.size() != KEY_RECORD_LEN) {
        return ESP_ERR_NOT_FOUND;
    }
    std::string password;
    if (pin) {
        password = *pin;
    } else if (!device_secret(password)) {
        return ESP_FAIL;
    }

    const uint8_t* p = record.data();
    uint32_t iterations = p[4] | (p[5] << 8) | (p[6] << 16) | ((uint32_t)p[7] << 24);
    const uint8_t* salt = p + 8;
    const uint8_t* iv = salt + SALT_LEN;
    const uint8_t* tag = iv + IV_LEN;
    const uint8_t* wrapped = tag + TAG_LEN;

    uint8_t kek[KEY_LEN];
    bool ok = iterations > 0 && iterations <= 1000000 &&
              derive_key((const uint8_t*)password.data(), password.size(), salt, iterations, kek) &&
              gcm_decrypt(kek, iv, "key", wrapped, KEY_LEN, tag, data_key);
    mbedtls_platform_zeroize(kek, sizeof(kek));
    vault::wipe(password);
    return ok ? ESP_OK : ESP_ERR_INVALID_ARG;
}

static bool record_has_pin()
{
    std::vector<uint8_t> record;
    return nvs_read_blob(NS_VAULT, "key", record) && record.size() == KEY_RECORD_LEN &&
           memcmp(record.data(), MAGIC_PIN, 4) == 0;
}

static esp_err_t create_new_vault()
{
    uint8_t data_key[KEY_LEN];
    esp_fill_random(data_key, KEY_LEN);
    esp_err_t err = write_key_record(NULL, data_key);
    if (err == ESP_OK) {
        memcpy(s_data_key, data_key, KEY_LEN);
        s_unlocked = true;
        s_has_pin = false;
    }
    mbedtls_platform_zeroize(data_key, sizeof(data_key));
    return err;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

bool vault::init()
{
    lock();
    s_available = false;

    std::vector<uint8_t> record;
    if (!nvs_read_blob(NS_VAULT, "key", record)) {
        s_available = create_new_vault() == ESP_OK;
        ESP_LOGI(TAG, "New vault created (automatic unlock)");
        return false;
    }
    s_available = true;

    if (record_has_pin()) {
        s_has_pin = true;          // Unlocked later with the PIN
        return false;
    }
    s_has_pin = false;

    uint8_t data_key[KEY_LEN];
    if (read_key_record(NULL, data_key) == ESP_OK) {
        memcpy(s_data_key, data_key, KEY_LEN);
        s_unlocked = true;
        mbedtls_platform_zeroize(data_key, sizeof(data_key));
        return false;
    }

    // Wrapped for a different device (e.g. restored backup): those passwords are unusable
    ESP_LOGW(TAG, "Vault was created on another device; starting a new one");
    reset();
    return true;
}

bool vault::is_available()
{
    return s_available;
}

bool vault::has_pin()
{
    return s_has_pin;
}

bool vault::is_unlocked()
{
    return s_unlocked;
}

esp_err_t vault::unlock(const std::string& pin)
{
    uint8_t data_key[KEY_LEN];
    esp_err_t err = read_key_record(&pin, data_key);
    if (err == ESP_OK) {
        memcpy(s_data_key, data_key, KEY_LEN);
        s_unlocked = true;
    }
    mbedtls_platform_zeroize(data_key, sizeof(data_key));
    return err;
}

void vault::lock()
{
    mbedtls_platform_zeroize(s_data_key, sizeof(s_data_key));
    s_unlocked = false;
}

esp_err_t vault::set_pin(const std::string& pin)
{
    if (!s_unlocked) {
        return ESP_ERR_INVALID_STATE;
    }
    if (pin.size() < VAULT_MIN_PIN_LENGTH) {
        return ESP_ERR_INVALID_SIZE;
    }
    esp_err_t err = write_key_record(&pin, s_data_key);
    if (err == ESP_OK) {
        s_has_pin = true;
    }
    return err;
}

esp_err_t vault::change_pin(const std::string& old_pin, const std::string& new_pin)
{
    if (new_pin.size() < VAULT_MIN_PIN_LENGTH) {
        return ESP_ERR_INVALID_SIZE;
    }
    esp_err_t err = unlock(old_pin);
    return err == ESP_OK ? set_pin(new_pin) : err;
}

esp_err_t vault::remove_pin(const std::string& pin)
{
    esp_err_t err = unlock(pin);
    if (err == ESP_OK) {
        err = write_key_record(NULL, s_data_key);
        if (err == ESP_OK) {
            s_has_pin = false;
        }
    }
    return err;
}

esp_err_t vault::reset()
{
    lock();
    nvs_erase(NS_VAULT, NULL);
    esp_err_t err = create_new_vault();
    s_available = err == ESP_OK;
    return err;
}

esp_err_t vault::put(const std::string& id, const std::string& secret)
{
    if (!s_unlocked) {
        return ESP_ERR_INVALID_STATE;
    }
    if (secret.size() > MAX_SECRET_LEN) {
        return ESP_ERR_INVALID_SIZE;
    }
    std::string name = secret_key_name(id);
    std::vector<uint8_t> record(IV_LEN + TAG_LEN + secret.size());
    uint8_t* iv = record.data();
    uint8_t* tag = iv + IV_LEN;
    esp_fill_random(iv, IV_LEN);
    if (!gcm_encrypt(s_data_key, iv, name, (const uint8_t*)secret.data(), secret.size(), tag + TAG_LEN, tag)) {
        return ESP_FAIL;
    }
    return nvs_write_blob(NS_VAULT, name.c_str(), record.data(), record.size());
}

esp_err_t vault::get(const std::string& id, std::string& secret)
{
    if (!s_unlocked) {
        return ESP_ERR_INVALID_STATE;
    }
    std::string name = secret_key_name(id);
    std::vector<uint8_t> record;
    if (!nvs_read_blob(NS_VAULT, name.c_str(), record)) {
        return ESP_ERR_NOT_FOUND;
    }
    if (record.size() < IV_LEN + TAG_LEN) {
        return ESP_FAIL;
    }
    const uint8_t* iv = record.data();
    const uint8_t* tag = iv + IV_LEN;
    size_t len = record.size() - IV_LEN - TAG_LEN;
    std::vector<uint8_t> plain(len);
    if (!gcm_decrypt(s_data_key, iv, name, tag + TAG_LEN, len, tag, plain.data())) {
        ESP_LOGE(TAG, "Secret failed authentication");
        return ESP_FAIL;
    }
    secret.assign((const char*)plain.data(), len);
    mbedtls_platform_zeroize(plain.data(), plain.size());
    return ESP_OK;
}

esp_err_t vault::remove(const std::string& id)
{
    nvs_erase(NS_VAULT, secret_key_name(id).c_str());
    return ESP_OK;
}

bool vault::has(const std::string& id)
{
    std::vector<uint8_t> record;
    return nvs_read_blob(NS_VAULT, secret_key_name(id).c_str(), record);
}

int vault::count()
{
    int n = 0;
    nvs_iterator_t it = NULL;
    esp_err_t err = nvs_entry_find(NVS_DEFAULT_PART_NAME, NS_VAULT, NVS_TYPE_BLOB, &it);
    while (err == ESP_OK) {
        nvs_entry_info_t info;
        nvs_entry_info(it, &info);
        if (info.key[0] == 's') {
            n++;
        }
        err = nvs_entry_next(&it);
    }
    nvs_release_iterator(it);
    return n;
}

void vault::wipe(std::string& s)
{
    if (!s.empty()) {
        mbedtls_platform_zeroize(&s[0], s.size());
    }
    s.clear();
}
