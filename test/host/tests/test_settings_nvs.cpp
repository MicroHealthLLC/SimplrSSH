/*
 * settings_nvs::init(): picks the partition that holds the user's settings and never erases
 * a launcher's own "nvs" (AI_RULES.md, Persistence).
 */

#include "helpers.hpp"

using settings_nvs::InitResult;
static const esp_partition_type_t DATA = ESP_PARTITION_TYPE_DATA;

TEST(nvs_factory_image_uses_nvs)
{
    fake::add_partition(DATA, ESP_PARTITION_SUBTYPE_DATA_NVS, 0x40000, "nvs");
    fake::set_running(ESP_PARTITION_SUBTYPE_APP_FACTORY);
    CHECK(settings_nvs::init() == InitResult::OK);
    CHECK_EQ(std::string(settings_nvs::partition()), std::string("nvs"));
    CHECK_EQ(fake::erase_count("nvs"), 0);
}

TEST(nvs_factory_image_reformats_unreadable_nvs)
{
    fake::add_partition(DATA, ESP_PARTITION_SUBTYPE_DATA_NVS, 0x40000, "nvs");
    fake::set_running(ESP_PARTITION_SUBTYPE_APP_FACTORY);
    fake::set_init_result("nvs", ESP_ERR_NVS_NO_FREE_PAGES);
    CHECK(settings_nvs::init() == InitResult::REFORMATTED);
    CHECK_EQ(fake::erase_count("nvs"), 1);
}

TEST(nvs_factory_image_other_error_not_erased)
{
    fake::add_partition(DATA, ESP_PARTITION_SUBTYPE_DATA_NVS, 0x40000, "nvs");
    fake::set_running(ESP_PARTITION_SUBTYPE_APP_FACTORY);
    fake::set_init_result("nvs", ESP_FAIL);
    CHECK(settings_nvs::init() == InitResult::UNAVAILABLE);
    CHECK_EQ(fake::erase_count("nvs"), 0);
}

TEST(nvs_launcher_partition_preferred)
{
    fake::add_partition(DATA, ESP_PARTITION_SUBTYPE_DATA_NVS, 0x5000, "nvs");
    fake::add_partition(DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, 0x40000, "simplrssh");
    fake::set_running(ESP_PARTITION_SUBTYPE_APP_OTA_0);
    CHECK(settings_nvs::init() == InitResult::OK);
    CHECK_EQ(std::string(settings_nvs::partition()), std::string("simplrssh"));
    CHECK_EQ(fake::erase_count("nvs"), 0);
}

TEST(nvs_launcher_partition_with_foreign_data_reformatted)
{
    // A launcher formats the partition it creates as SPIFFS/FAT: not NVS yet, so it is ours to format
    fake::add_partition(DATA, ESP_PARTITION_SUBTYPE_DATA_NVS, 0x5000, "nvs");
    fake::add_partition(DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, 0x40000, "simplrssh");
    fake::set_running(ESP_PARTITION_SUBTYPE_APP_OTA_0);
    fake::set_init_result("simplrssh", ESP_FAIL);
    CHECK(settings_nvs::init() == InitResult::REFORMATTED);
    CHECK_EQ(fake::erase_count("simplrssh"), 1);
    CHECK_EQ(fake::erase_count("nvs"), 0);
}

TEST(nvs_legacy_pocketssh_partition_kept)
{
    // Installed before the rename, then updated: the old partition still holds the settings
    fake::add_partition(DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, 0x40000, "simplrssh");
    fake::add_partition(DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, 0x40000, "pocketssh");
    fake::set_running(ESP_PARTITION_SUBTYPE_APP_OTA_0);
    CHECK(settings_nvs::init() == InitResult::OK);
    CHECK_EQ(std::string(settings_nvs::partition()), std::string("pocketssh"));
    CHECK_EQ(fake::erase_count("pocketssh"), 0);
}

TEST(nvs_tiny_own_partition_ignored)
{
    fake::add_partition(DATA, ESP_PARTITION_SUBTYPE_DATA_NVS, 0x40000, "nvs");
    fake::add_partition(DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, 0x1000, "simplrssh");
    fake::set_running(ESP_PARTITION_SUBTYPE_APP_FACTORY);
    CHECK(settings_nvs::init() == InitResult::OK);
    CHECK_EQ(std::string(settings_nvs::partition()), std::string("nvs"));
}

TEST(nvs_launcher_without_own_partition_shares_nvs)
{
    fake::add_partition(DATA, ESP_PARTITION_SUBTYPE_DATA_NVS, 0x5000, "nvs");
    fake::set_running(ESP_PARTITION_SUBTYPE_APP_OTA_0);
    CHECK(settings_nvs::init() == InitResult::SHARED);
    CHECK_EQ(std::string(settings_nvs::partition()), std::string("nvs"));
}

TEST(nvs_launcher_nvs_never_erased)
{
    fake::add_partition(DATA, ESP_PARTITION_SUBTYPE_DATA_NVS, 0x5000, "nvs");
    fake::set_running(ESP_PARTITION_SUBTYPE_APP_OTA_0);
    fake::set_init_result("nvs", ESP_ERR_NVS_NO_FREE_PAGES);
    CHECK(settings_nvs::init() == InitResult::UNAVAILABLE);
    CHECK_EQ(fake::erase_count("nvs"), 0);
}

TEST(nvs_missing_partition_unavailable)
{
    fake::set_running(ESP_PARTITION_SUBTYPE_APP_FACTORY);
    CHECK(settings_nvs::init() == InitResult::UNAVAILABLE);
}

TEST(nvs_open_uses_active_partition)
{
    fake::add_partition(DATA, ESP_PARTITION_SUBTYPE_DATA_NVS, 0x5000, "nvs");
    fake::add_partition(DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, 0x40000, "simplrssh");
    fake::set_running(ESP_PARTITION_SUBTYPE_APP_OTA_0);
    fake::init_partition("nvs");
    CHECK(settings_nvs::init() == InitResult::OK);
    nvs_handle_t h;
    CHECK_EQ(settings_nvs::open("profiles", NVS_READWRITE, &h), ESP_OK);
    CHECK_EQ(nvs_set_u32(h, "count", 1), ESP_OK);
    nvs_close(h);
    CHECK_EQ(fake::store()["simplrssh"]["profiles"].size(), (size_t)1);
    CHECK_EQ(fake::store()["nvs"].count("profiles"), (size_t)0);
}
