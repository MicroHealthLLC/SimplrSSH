/*
 * Shared setup for the host tests.
 */

#pragma once
#include "fake_idf.hpp"
#include "settings_nvs.hpp"
#include "test.hpp"
#include <string>

// A T-Deck flashed with the merged release image: "nvs" after the factory app
inline void factory_device()
{
    fake::add_partition(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_NVS, 0x40000, "nvs");
    fake::set_running(ESP_PARTITION_SUBTYPE_APP_FACTORY);
    CHECK(settings_nvs::init() == settings_nvs::InitResult::OK);
}

inline bool log_contains(const std::string& needle)
{
    for (const auto& line : fake::log()) {
        if (line.find(needle) != std::string::npos) {
            return true;
        }
    }
    return false;
}

inline std::string hex_of(const std::string& s)
{
    static const char* digits = "0123456789abcdef";
    std::string out;
    for (unsigned char c : s) {
        out += digits[c >> 4];
        out += digits[c & 0xF];
    }
    return out;
}
