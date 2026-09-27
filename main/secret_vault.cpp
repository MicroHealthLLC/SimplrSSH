/*
 * Secret Vault Implementation
 *
 * Files on the storage partition:
 *   vault.key          "PSK1" | iterations (u32 LE) | salt[16] | iv[12] | tag[16] | wrapped data key[32]
 *   s<16 hex>.enc      "PSE1" | iv[12] | tag[16] | ciphertext
 * Secret file names are a truncated SHA-256 of the secret id (so SSIDs and
 * profile names don't appear on flash), and the file name is bound to the
 * ciphertext as GCM additional data.
 */

#include "secret_vault.hpp"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_spiffs.h"
#include "mbedtls/gcm.h"
#include "mbedtls/md.h"
#include "mbedtls/platform_util.h"
#include <cstdio>
#include <cstring>
#include <cctype>
#include <vector>
#include <dirent.h>

static const char *TAG = "VAULT";

static const char KEY_MAGIC[4] = {'P', 'S', 'K', '1'};
static const char SECRET_MAGIC[4] = {'P', 'S', 'E', '1'};
static const uint32_t PBKDF2_ITERATIONS = 20000;
static const size_t SALT_LEN = 16;
static const size_t IV_LEN = 12;
static const size_t TAG_LEN = 16;
static const size_t KEY_LEN = 32;
static const size_t KEY_FILE_LEN = 4 + 4 + SALT_LEN + IV_LEN + TAG_LEN + KEY_LEN;
static const size_t MAX_SECRET_LEN = 256;

static std::string s_base_path;
static bool s_mounted = false;
static bool s_unlocked = false;
static uint8_t s_data_key[KEY_LEN];

static std::string path_for(const char* file_name)
{
    return s_base_path + "/" + file_name;
}

static bool read_file(const std::string& path, std::vector<uint8_t>& out)
{
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) {
        return false;
    }
    out.clear();
    uint8_t buf[128];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        out.insert(out.end(), buf, buf + n);
        if (out.size() > 4096) {
            break;
        }
    }
    fclose(f);
    return true;
}

static bool write_file(const std::string& path, const std::vector<uint8_t>& data)
{
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) {
        return false;
    }
    bool ok = fwrite(data.data(), 1, data.size(), f) == data.size();
    ok = (fclose(f) == 0) && ok;
    return ok;
}

// PBKDF2-HMAC-SHA256 producing a single 32-byte block
static bool derive_key(const std::string& pin, const uint8_t* salt, uint32_t iterations, uint8_t* out)
{
    const mbedtls_md_info_t* info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    mbedtls_md_context_t ctx;
    mbedtls_md_init(&ctx);

    uint8_t u[KEY_LEN];
    const uint8_t block_index[4] = {0, 0, 0, 1};
    bool ok = mbedtls_md_setup(&ctx, info, 1) == 0 &&
              mbedtls_md_hmac_starts(&ctx, (const uint8_t*)pin.data(), pin.size()) == 0 &&
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

static std::string secret_file_name(const std::string& id)
{
    uint8_t hash[32];
    mbedtls_md(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), (const uint8_t*)id.data(), id.size(), hash);
    char name[24];
    snprintf(name, sizeof(name), "s%02x%02x%02x%02x%02x%02x%02x%02x.enc",
             hash[0], hash[1], hash[2], hash[3], hash[4], hash[5], hash[6], hash[7]);
    return name;
}

// Wrap the data key under a PIN-derived key and write vault.key (via a temp file)
static esp_err_t write_key_file(const std::string& pin, const uint8_t* data_key)
{
    std::vector<uint8_t> file(KEY_FILE_LEN);
    uint8_t* p = file.data();
    memcpy(p, KEY_MAGIC, 4);
    p[4] = PBKDF2_ITERATIONS & 0xFF;
    p[5] = (PBKDF2_ITERATIONS >> 8) & 0xFF;
    p[6] = (PBKDF2_ITERATIONS >> 16) & 0xFF;
    p[7] = (PBKDF2_ITERATIONS >> 24) & 0xFF;
    uint8_t* salt = p + 8;
    uint8_t* iv = salt + SALT_LEN;
    uint8_t* tag = iv + IV_LEN;
    uint8_t* wrapped = tag + TAG_LEN;

    esp_fill_random(salt, SALT_LEN);
    esp_fill_random(iv, IV_LEN);

    uint8_t kek[KEY_LEN];
    bool ok = derive_key(pin, salt, PBKDF2_ITERATIONS, kek) &&
              gcm_encrypt(kek, iv, "vault.key", data_key, KEY_LEN, wrapped, tag);
    mbedtls_platform_zeroize(kek, sizeof(kek));
    if (!ok) {
        return ESP_FAIL;
    }

    std::string tmp_path = path_for("vault.new");
    std::string key_path = path_for("vault.key");
    if (!write_file(tmp_path, file)) {
        ESP_LOGE(TAG, "Failed to write key file");
        return ESP_FAIL;
    }
    ::remove(key_path.c_str());
    if (rename(tmp_path.c_str(), key_path.c_str()) != 0) {
        ESP_LOGE(TAG, "Failed to replace key file");
        return ESP_FAIL;
    }
    return ESP_OK;
}

static esp_err_t unwrap_data_key(const std::string& pin, uint8_t* data_key)
{
    std::vector<uint8_t> file;
    if (!read_file(path_for("vault.key"), file) || file.size() != KEY_FILE_LEN ||
        memcmp(file.data(), KEY_MAGIC, 4) != 0) {
        ESP_LOGE(TAG, "Key file missing or corrupt");
        return ESP_ERR_NOT_FOUND;
    }

    const uint8_t* p = file.data();
    uint32_t iterations = p[4] | (p[5] << 8) | (p[6] << 16) | ((uint32_t)p[7] << 24);
    const uint8_t* salt = p + 8;
    const uint8_t* iv = salt + SALT_LEN;
    const uint8_t* tag = iv + IV_LEN;
    const uint8_t* wrapped = tag + TAG_LEN;

    uint8_t kek[KEY_LEN];
    if (!derive_key(pin, salt, iterations, kek)) {
        return ESP_FAIL;
    }
    bool ok = gcm_decrypt(kek, iv, "vault.key", wrapped, KEY_LEN, tag, data_key);
    mbedtls_platform_zeroize(kek, sizeof(kek));
    return ok ? ESP_OK : ESP_ERR_INVALID_ARG;
}

esp_err_t vault::init(const char* base_path, const char* partition_label)
{
    s_base_path = base_path;

    esp_vfs_spiffs_conf_t conf = {};
    conf.base_path = base_path;
    conf.partition_label = partition_label;
    conf.max_files = 4;
    conf.format_if_mount_failed = true;  // The partition is dedicated to the vault

    esp_err_t err = esp_vfs_spiffs_register(&conf);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "Failed to mount %s partition: %s", partition_label, esp_err_to_name(err));
        return err;
    }
    s_mounted = true;

    // Finish a PIN change that was interrupted between writing and renaming
    FILE* f = fopen(path_for("vault.key").c_str(), "rb");
    if (f) {
        fclose(f);
    } else {
        rename(path_for("vault.new").c_str(), path_for("vault.key").c_str());
    }

    ESP_LOGI(TAG, "Vault storage mounted at %s (%s)", base_path, is_set_up() ? "PIN set" : "no PIN yet");
    return ESP_OK;
}

bool vault::is_mounted()
{
    return s_mounted;
}

bool vault::is_set_up()
{
    if (!s_mounted) {
        return false;
    }
    FILE* f = fopen(path_for("vault.key").c_str(), "rb");
    if (f) {
        fclose(f);
        return true;
    }
    return false;
}

bool vault::is_unlocked()
{
    return s_unlocked;
}

esp_err_t vault::setup(const std::string& pin)
{
    if (!s_mounted) {
        return ESP_ERR_INVALID_STATE;
    }
    if (pin.size() < VAULT_MIN_PIN_LENGTH) {
        return ESP_ERR_INVALID_SIZE;
    }

    uint8_t data_key[KEY_LEN];
    esp_fill_random(data_key, KEY_LEN);
    esp_err_t err = write_key_file(pin, data_key);
    if (err == ESP_OK) {
        memcpy(s_data_key, data_key, KEY_LEN);
        s_unlocked = true;
        ESP_LOGI(TAG, "Vault created");
    }
    mbedtls_platform_zeroize(data_key, sizeof(data_key));
    return err;
}

esp_err_t vault::unlock(const std::string& pin)
{
    if (!is_set_up()) {
        return ESP_ERR_INVALID_STATE;
    }
    uint8_t data_key[KEY_LEN];
    esp_err_t err = unwrap_data_key(pin, data_key);
    if (err == ESP_OK) {
        memcpy(s_data_key, data_key, KEY_LEN);
        s_unlocked = true;
        ESP_LOGI(TAG, "Vault unlocked");
    } else if (err == ESP_ERR_INVALID_ARG) {
        ESP_LOGW(TAG, "Wrong PIN");
    }
    mbedtls_platform_zeroize(data_key, sizeof(data_key));
    return err;
}

void vault::lock()
{
    mbedtls_platform_zeroize(s_data_key, sizeof(s_data_key));
    s_unlocked = false;
}

esp_err_t vault::change_pin(const std::string& old_pin, const std::string& new_pin)
{
    if (new_pin.size() < VAULT_MIN_PIN_LENGTH) {
        return ESP_ERR_INVALID_SIZE;
    }
    uint8_t data_key[KEY_LEN];
    esp_err_t err = unwrap_data_key(old_pin, data_key);
    if (err == ESP_OK) {
        err = write_key_file(new_pin, data_key);
        if (err == ESP_OK) {
            memcpy(s_data_key, data_key, KEY_LEN);
            s_unlocked = true;
            ESP_LOGI(TAG, "PIN changed");
        }
    }
    mbedtls_platform_zeroize(data_key, sizeof(data_key));
    return err;
}

esp_err_t vault::reset()
{
    if (!s_mounted) {
        return ESP_ERR_INVALID_STATE;
    }
    lock();

    DIR* dir = opendir(s_base_path.c_str());
    if (dir) {
        std::vector<std::string> names;
        struct dirent* entry;
        while ((entry = readdir(dir)) != NULL) {
            std::string name = entry->d_name;
            bool secret = name.size() > 4 && name[0] == 's' && name.compare(name.size() - 4, 4, ".enc") == 0;
            if (secret || name == "vault.key" || name == "vault.new") {
                names.push_back(name);
            }
        }
        closedir(dir);
        for (const auto& name : names) {
            ::remove(path_for(name.c_str()).c_str());
        }
    }
    ESP_LOGI(TAG, "Vault reset");
    return ESP_OK;
}

esp_err_t vault::put(const std::string& id, const std::string& secret)
{
    if (!s_unlocked) {
        return ESP_ERR_INVALID_STATE;
    }
    if (secret.size() > MAX_SECRET_LEN) {
        return ESP_ERR_INVALID_SIZE;
    }

    std::string name = secret_file_name(id);
    std::vector<uint8_t> file(4 + IV_LEN + TAG_LEN + secret.size());
    uint8_t* iv = file.data() + 4;
    uint8_t* tag = iv + IV_LEN;
    uint8_t* ct = tag + TAG_LEN;
    memcpy(file.data(), SECRET_MAGIC, 4);
    esp_fill_random(iv, IV_LEN);

    if (!gcm_encrypt(s_data_key, iv, name, (const uint8_t*)secret.data(), secret.size(), ct, tag)) {
        return ESP_FAIL;
    }
    if (!write_file(path_for(name.c_str()), file)) {
        ESP_LOGE(TAG, "Failed to write secret file");
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t vault::get(const std::string& id, std::string& secret)
{
    if (!s_unlocked) {
        return ESP_ERR_INVALID_STATE;
    }

    std::string name = secret_file_name(id);
    std::vector<uint8_t> file;
    if (!read_file(path_for(name.c_str()), file)) {
        return ESP_ERR_NOT_FOUND;
    }
    if (file.size() < 4 + IV_LEN + TAG_LEN || memcmp(file.data(), SECRET_MAGIC, 4) != 0) {
        return ESP_FAIL;
    }

    const uint8_t* iv = file.data() + 4;
    const uint8_t* tag = iv + IV_LEN;
    const uint8_t* ct = tag + TAG_LEN;
    size_t len = file.size() - (4 + IV_LEN + TAG_LEN);

    std::vector<uint8_t> plain(len);
    if (!gcm_decrypt(s_data_key, iv, name, ct, len, tag, plain.data())) {
        ESP_LOGE(TAG, "Secret failed authentication");
        return ESP_FAIL;
    }
    secret.assign((const char*)plain.data(), len);
    mbedtls_platform_zeroize(plain.data(), plain.size());
    return ESP_OK;
}

esp_err_t vault::remove(const std::string& id)
{
    if (!s_mounted) {
        return ESP_ERR_INVALID_STATE;
    }
    ::remove(path_for(secret_file_name(id).c_str()).c_str());
    return ESP_OK;
}

bool vault::has(const std::string& id)
{
    if (!s_mounted) {
        return false;
    }
    FILE* f = fopen(path_for(secret_file_name(id).c_str()).c_str(), "rb");
    if (f) {
        fclose(f);
        return true;
    }
    return false;
}

bool vault::is_vault_file_name(const std::string& name)
{
    if (name == "vault.key") {
        return true;
    }
    if (name.size() != 21 || name[0] != 's' || name.compare(17, 4, ".enc") != 0) {
        return false;
    }
    for (size_t i = 1; i < 17; i++) {
        if (!isxdigit((unsigned char)name[i]) || isupper((unsigned char)name[i])) {
            return false;
        }
    }
    return true;
}

std::vector<std::string> vault::list_files()
{
    std::vector<std::string> names;
    DIR* dir = s_mounted ? opendir(s_base_path.c_str()) : NULL;
    if (dir) {
        struct dirent* entry;
        while ((entry = readdir(dir)) != NULL) {
            if (is_vault_file_name(entry->d_name)) {
                names.push_back(entry->d_name);
            }
        }
        closedir(dir);
    }
    return names;
}

esp_err_t vault::read_raw(const std::string& name, std::vector<uint8_t>& data)
{
    if (!s_mounted || !is_vault_file_name(name)) {
        return ESP_ERR_INVALID_ARG;
    }
    return read_file(path_for(name.c_str()), data) ? ESP_OK : ESP_ERR_NOT_FOUND;
}

esp_err_t vault::write_raw(const std::string& name, const std::vector<uint8_t>& data)
{
    if (!s_mounted || !is_vault_file_name(name) || data.size() > 4096) {
        return ESP_ERR_INVALID_ARG;
    }
    return write_file(path_for(name.c_str()), data) ? ESP_OK : ESP_FAIL;
}

void vault::wipe(std::string& s)
{
    if (!s.empty()) {
        mbedtls_platform_zeroize(&s[0], s.size());
    }
    s.clear();
}
