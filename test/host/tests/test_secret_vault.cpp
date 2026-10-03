/*
 * vault: AES-256-GCM secrets in NVS, optional PIN, nothing readable in flash or logs.
 */

#include "secret_vault.hpp"
#include "helpers.hpp"

static const std::string SECRET = "correct horse battery staple";
static const std::string PIN = "4711";

static void fresh_vault()
{
    factory_device();
    CHECK(!vault::init());
    CHECK(vault::is_available());
    CHECK(vault::is_unlocked());
    CHECK(!vault::has_pin());
}

TEST(vault_put_get_round_trip)
{
    fresh_vault();
    CHECK_EQ(vault::put("wifi:HomeNet", SECRET), ESP_OK);
    std::string out;
    CHECK_EQ(vault::get("wifi:HomeNet", out), ESP_OK);
    CHECK_EQ(out, SECRET);
    CHECK(vault::has("wifi:HomeNet"));
    CHECK(!vault::has("wifi:Other"));
    CHECK_EQ(vault::count(), 1);
    CHECK_EQ(fake::open_handles(), 0);
}

TEST(vault_missing_secret_not_found)
{
    fresh_vault();
    std::string out;
    CHECK_EQ(vault::get("profile:none", out), ESP_ERR_NOT_FOUND);
}

TEST(vault_remove_secret)
{
    fresh_vault();
    CHECK_EQ(vault::put("profile:1", SECRET), ESP_OK);
    CHECK_EQ(vault::remove("profile:1"), ESP_OK);
    CHECK(!vault::has("profile:1"));
    CHECK_EQ(vault::count(), 0);
}

TEST(vault_no_plaintext_in_flash)
{
    fresh_vault();
    CHECK_EQ(vault::put("wifi:HomeNet", SECRET), ESP_OK);
    std::string flash = fake::dump_all();
    CHECK(flash.find(SECRET) == std::string::npos);
    CHECK(flash.find("HomeNet") == std::string::npos);   // Ids are hashed into key names
}

TEST(vault_survives_reboot_without_pin)
{
    fresh_vault();
    CHECK_EQ(vault::put("wifi:HomeNet", SECRET), ESP_OK);
    CHECK(!vault::init());            // Reboot: unlocks by itself
    CHECK(vault::is_unlocked());
    std::string out;
    CHECK_EQ(vault::get("wifi:HomeNet", out), ESP_OK);
    CHECK_EQ(out, SECRET);
}

TEST(vault_rejects_oversized_secret)
{
    fresh_vault();
    CHECK_EQ(vault::put("profile:1", std::string(257, 'x')), ESP_ERR_INVALID_SIZE);
    CHECK_EQ(vault::put("profile:1", std::string(256, 'x')), ESP_OK);
}

TEST(vault_tampered_secret_fails_authentication)
{
    fresh_vault();
    CHECK_EQ(vault::put("profile:1", SECRET), ESP_OK);
    for (auto& kv : fake::store()["nvs"]["vault"]) {
        if (kv.first[0] == 's') {
            kv.second.bytes.back() ^= 0x01;
        }
    }
    std::string out;
    CHECK(vault::get("profile:1", out) != ESP_OK);
    CHECK(out.empty());
}

TEST(vault_secret_bound_to_its_id)
{
    fresh_vault();
    CHECK_EQ(vault::put("profile:a", SECRET), ESP_OK);
    CHECK_EQ(vault::put("profile:b", "other"), ESP_OK);
    // Move a's record under b's key name: the id is authenticated data, so it must not decrypt
    auto& ns = fake::store()["nvs"]["vault"];
    std::string a_key;
    std::string b_key;
    for (const auto& kv : ns) {
        if (kv.first[0] == 's' && kv.second.bytes.size() == 12 + 16 + SECRET.size()) {
            a_key = kv.first;
        } else if (kv.first[0] == 's') {
            b_key = kv.first;
        }
    }
    CHECK(!a_key.empty() && !b_key.empty());
    ns[b_key] = ns[a_key];
    std::string out;
    CHECK(vault::get("profile:b", out) != ESP_OK);
}

TEST(vault_pin_locks_after_reboot)
{
    fresh_vault();
    CHECK_EQ(vault::put("wifi:HomeNet", SECRET), ESP_OK);
    CHECK_EQ(vault::set_pin("12"), ESP_ERR_INVALID_SIZE);
    CHECK_EQ(vault::set_pin(PIN), ESP_OK);
    CHECK(vault::has_pin());

    CHECK(!vault::init());            // Reboot
    CHECK(vault::has_pin());
    CHECK(!vault::is_unlocked());
    std::string out;
    CHECK_EQ(vault::get("wifi:HomeNet", out), ESP_ERR_INVALID_STATE);
    CHECK_EQ(vault::put("wifi:Other", SECRET), ESP_ERR_INVALID_STATE);

    CHECK_EQ(vault::unlock("0000"), ESP_ERR_INVALID_ARG);
    CHECK(!vault::is_unlocked());
    CHECK_EQ(vault::unlock(PIN), ESP_OK);
    CHECK_EQ(vault::get("wifi:HomeNet", out), ESP_OK);
    CHECK_EQ(out, SECRET);
}

TEST(vault_pin_uses_slow_key_derivation)
{
    fresh_vault();
    CHECK_EQ(vault::set_pin(PIN), ESP_OK);
    const std::string& record = fake::store()["nvs"]["vault"]["key"].bytes;
    CHECK_EQ(record.substr(0, 4), std::string("PSK1"));
    uint32_t iterations = (uint8_t)record[4] | ((uint8_t)record[5] << 8) | ((uint8_t)record[6] << 16) |
                          ((uint32_t)(uint8_t)record[7] << 24);
    CHECK(iterations >= 20000);
}

TEST(vault_change_and_remove_pin)
{
    fresh_vault();
    CHECK_EQ(vault::put("profile:1", SECRET), ESP_OK);
    CHECK_EQ(vault::set_pin(PIN), ESP_OK);
    CHECK_EQ(vault::change_pin("9999", "123456"), ESP_ERR_INVALID_ARG);
    CHECK_EQ(vault::change_pin(PIN, "12"), ESP_ERR_INVALID_SIZE);
    CHECK_EQ(vault::change_pin(PIN, "123456"), ESP_OK);
    vault::lock();
    CHECK_EQ(vault::unlock(PIN), ESP_ERR_INVALID_ARG);
    CHECK_EQ(vault::unlock("123456"), ESP_OK);

    CHECK_EQ(vault::remove_pin("123456"), ESP_OK);
    CHECK(!vault::has_pin());
    CHECK(!vault::init());            // Reboot: automatic unlock again
    CHECK(vault::is_unlocked());
    std::string out;
    CHECK_EQ(vault::get("profile:1", out), ESP_OK);
    CHECK_EQ(out, SECRET);
}

TEST(vault_reset_deletes_every_secret)
{
    fresh_vault();
    CHECK_EQ(vault::put("profile:1", SECRET), ESP_OK);
    CHECK_EQ(vault::set_pin(PIN), ESP_OK);
    CHECK_EQ(vault::reset(), ESP_OK);
    CHECK(!vault::has_pin());
    CHECK(vault::is_unlocked());
    CHECK_EQ(vault::count(), 0);
    std::string out;
    CHECK_EQ(vault::get("profile:1", out), ESP_ERR_NOT_FOUND);
}

TEST(vault_secrets_and_pin_never_logged)
{
    fresh_vault();
    CHECK_EQ(vault::put("wifi:HomeNet", SECRET), ESP_OK);
    CHECK_EQ(vault::set_pin(PIN), ESP_OK);
    vault::init();
    vault::unlock("0000");
    vault::unlock(PIN);
    std::string out;
    vault::get("wifi:HomeNet", out);
    CHECK(!log_contains(SECRET));
    CHECK(!log_contains(PIN));
    CHECK(!log_contains("HomeNet"));
}

TEST(vault_unavailable_without_settings_partition)
{
    // No partition initialized: nothing can be saved, and the vault says so
    CHECK(!vault::init());
    CHECK(!vault::is_available());
    CHECK_EQ(vault::put("profile:1", SECRET), ESP_ERR_INVALID_STATE);
}

TEST(vault_wipe_clears_string)
{
    std::string s = SECRET;
    vault::wipe(s);
    CHECK(s.empty());
}
