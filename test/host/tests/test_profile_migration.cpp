/*
 * Records written by older firmware still load; plain-text passwords from before the vault
 * are removed from flash on first load.
 */

#include "connection_profiles.hpp"
#include "helpers.hpp"

// Fields are written with '|' here and stored with the real separator (0x1F)
static void put_record(const char* ns, const char* key, std::string value, uint32_t count)
{
    for (char& c : value) {
        if (c == '|') {
            c = '\x1F';
        }
    }
    nvs_handle_t h;
    CHECK_EQ(settings_nvs::open(ns, NVS_READWRITE, &h), ESP_OK);
    CHECK_EQ(nvs_set_str(h, key, value.c_str()), ESP_OK);
    CHECK_EQ(nvs_set_u32(h, "count", count), ESP_OK);
    nvs_close(h);
}

TEST(migration_v1_profile_drops_plaintext_password)
{
    factory_device();
    put_record("profiles", "p0", "1|my server|host.example|2222|pi|p|plainpass1|", 1);

    auto loaded = profile_store::load();
    CHECK_EQ(loaded.size(), (size_t)1);
    if (!loaded.empty()) {
        CHECK_EQ(loaded[0].name, std::string("my server"));
        CHECK_EQ(loaded[0].host, std::string("host.example"));
        CHECK_EQ(loaded[0].port, 2222);
        CHECK_EQ(loaded[0].username, std::string("pi"));
        CHECK_EQ(loaded[0].id.size(), (size_t)8);
        CHECK(!loaded[0].secret_saved);
    }
    CHECK(fake::dump_all().find("plainpass1") == std::string::npos);
    CHECK_EQ(fake::store()["nvs"]["profiles"]["p0"].bytes.substr(0, 2), std::string("2\x1F"));
}

TEST(migration_v1_key_profile_keeps_key_name)
{
    factory_device();
    put_record("profiles", "p0", "1|box|10.0.0.9|22|root|k||id_rsa.pem", 1);
    auto loaded = profile_store::load();
    CHECK_EQ(loaded.size(), (size_t)1);
    if (!loaded.empty()) {
        CHECK(loaded[0].auth == ConnectionProfile::Auth::Key);
        CHECK_EQ(loaded[0].key_name, std::string("id_rsa.pem"));
    }
}

TEST(migration_v1_network_loads_as_visible)
{
    factory_device();
    put_record("wifi_nets", "n0", "1|HomeNet|1", 1);
    auto loaded = network_store::load();
    CHECK_EQ(loaded.size(), (size_t)1);
    if (!loaded.empty()) {
        CHECK_EQ(loaded[0].ssid, std::string("HomeNet"));
        CHECK(loaded[0].secret_saved);
        CHECK(!loaded[0].hidden);
    }
}

TEST(migration_unknown_record_version_skipped)
{
    factory_device();
    put_record("wifi_nets", "n0", "9|Future|1|0|x", 1);
    CHECK(network_store::load().empty());
}
