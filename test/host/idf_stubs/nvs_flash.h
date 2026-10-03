// Host stand-in for ESP-IDF's nvs_flash.h
#pragma once
#include "nvs.h"
#include "esp_partition.h"

esp_err_t nvs_flash_init_partition_ptr(const esp_partition_t* partition);
esp_err_t nvs_flash_erase_partition_ptr(const esp_partition_t* partition);
