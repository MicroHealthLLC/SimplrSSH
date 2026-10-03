/*
 * Test controls for the ESP-IDF stand-ins (idf_stubs/): an in-memory NVS with any number of
 * partitions, a fake partition table and running app, and the captured log.
 */

#pragma once
#include "esp_partition.h"
#include "nvs.h"
#include <map>
#include <string>
#include <vector>

namespace fake
{
    struct Value {
        nvs_type_t type;
        std::string bytes;   // String (without NUL), blob bytes, or 4 little-endian bytes
    };
    // partition label -> namespace -> key -> value
    using Store = std::map<std::string, std::map<std::string, std::map<std::string, Value>>>;

    void reset();                                  // Empty store, table, log; seed the RNG
    void seed(uint32_t value);                     // Another device: different random secrets
    Store& store();                                // Every value written, for inspection
    void init_partition(const char* label);        // Mark a partition ready (nvs_flash_init done)
    std::string dump_all();                        // Every stored key and value, concatenated
    int open_handles();                            // Handles opened and not closed

    // Partition table and the running app (for settings_nvs::init())
    void add_partition(esp_partition_type_t type, esp_partition_subtype_t subtype, uint32_t size,
                       const char* label);
    void set_running(esp_partition_subtype_t app_subtype);   // Adds an app partition and runs it
    void set_init_result(const char* label, esp_err_t first_result);  // First init returns this
    int erase_count(const char* label);            // nvs_flash_erase_partition_ptr() calls

    void fail_writes(bool fail);                   // Every set/erase returns NOT_ENOUGH_SPACE
    const std::vector<std::string>& log();         // "E TAG message" per ESP_LOGx line
}
