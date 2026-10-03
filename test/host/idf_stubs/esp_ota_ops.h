// Host stand-in for ESP-IDF's esp_ota_ops.h
#pragma once
#include "esp_partition.h"

const esp_partition_t* esp_ota_get_running_partition(void);
