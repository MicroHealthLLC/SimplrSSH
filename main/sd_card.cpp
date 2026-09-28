/*
 * SD Card
 * Mounting the T-Deck SD card (SPI, CS on GPIO 39) at boot and at runtime, and
 * loading SSH private keys from /sdcard/ssh_keys.
 */

#include "sd_card.hpp"
#include "ssh_terminal.hpp"
#include "utilities.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "diskio_impl.h"
#include "diskio_sdmmc.h"
#include "esp_rom_gpio.h"
#include "driver/gpio.h"
#include "driver/sdspi_host.h"
#include "driver/spi_common.h"
#include "soc/spi_periph.h"
#include <cstring>
#include <cstdlib>
#include <dirent.h>
#include <sys/stat.h>

static const char *TAG = "SD_CARD";

const char* const sdcard::MOUNT_POINT = "/sdcard";

// The display (esp_bsp_generic) owns SPI2 on the shared SCK/MOSI pins
static const spi_host_device_t DISPLAY_SPI_HOST = SPI2_HOST;
static const spi_host_device_t BOOT_SPI_HOST = SPI3_HOST;

static sdmmc_card_t* s_card = NULL;
static bool s_owns_bus = false;

// The SD driver leaves CS as a floating input when it releases the card. The display
// keeps clocking data on the same SCK/MOSI lines, so a floating CS lets the card take
// pixel data as commands (including writes) and corrupt the file system. Keep it high.
static void deselect_card()
{
    gpio_set_level(BOARD_SDCARD_CS, 1);  // Level first, so CS never pulses low
    gpio_config_t out = {};
    out.pin_bit_mask = 1ULL << BOARD_SDCARD_CS;
    out.mode = GPIO_MODE_OUTPUT;
    out.pull_up_en = GPIO_PULLUP_ENABLE;
    gpio_config(&out);
}

// Read-only disk driver: reads go to the card, writes are refused before they reach it
static DSTATUS ro_status(BYTE pdrv)
{
    return STA_PROTECT;
}

static DRESULT ro_read(BYTE pdrv, BYTE* buff, DWORD sector, UINT count)
{
    return sdmmc_read_sectors(s_card, buff, sector, count) == ESP_OK ? RES_OK : RES_ERROR;
}

static DRESULT ro_write(BYTE pdrv, const BYTE* buff, DWORD sector, UINT count)
{
    return RES_WRPRT;
}

static DRESULT ro_ioctl(BYTE pdrv, BYTE cmd, void* buff)
{
    switch (cmd) {
        case CTRL_SYNC:
            return RES_OK;
        case GET_SECTOR_COUNT:
            *((DWORD*)buff) = s_card->csd.capacity;
            return RES_OK;
        case GET_SECTOR_SIZE:
            *((WORD*)buff) = s_card->csd.sector_size;
            return RES_OK;
        default:
            return RES_WRPRT;
    }
}

static const ff_diskio_impl_t s_read_only_impl = {
    .init = &ro_status,
    .status = &ro_status,
    .read = &ro_read,
    .write = &ro_write,
    .ioctl = &ro_ioctl,
};

static esp_err_t mount_on(spi_host_device_t host, int max_freq_khz, bool writable)
{
    esp_vfs_fat_sdmmc_mount_config_t mount_config = {};
    mount_config.format_if_mount_failed = false;  // Never format the user's card
    mount_config.max_files = 4;
    mount_config.allocation_unit_size = 16 * 1024;

    sdmmc_host_t sd_host = SDSPI_HOST_DEFAULT();
    sd_host.max_freq_khz = max_freq_khz;

    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_config.gpio_cs = BOARD_SDCARD_CS;
    slot_config.host_id = host;

    esp_err_t err = esp_vfs_fat_sdspi_mount(sdcard::MOUNT_POINT, &sd_host, &slot_config, &mount_config, &s_card);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "No SD card (%s)", esp_err_to_name(err));
        s_card = NULL;
        deselect_card();
        return err;
    }
    if (!writable) {
        // Mounting only reads the card; from here on FATFS refuses any change (FR_WRITE_PROTECTED)
        ff_diskio_register(ff_diskio_get_pdrv_card(s_card), &s_read_only_impl);
    }
    return ESP_OK;
}

esp_err_t sdcard::mount_at_boot()
{
    spi_bus_config_t bus_cfg = {};
    bus_cfg.mosi_io_num = BOARD_SPI_MOSI;
    bus_cfg.miso_io_num = BOARD_SPI_MISO;
    bus_cfg.sclk_io_num = BOARD_SPI_SCK;
    bus_cfg.quadwp_io_num = -1;
    bus_cfg.quadhd_io_num = -1;
    bus_cfg.max_transfer_sz = 4000;

    esp_err_t err = spi_bus_initialize(BOOT_SPI_HOST, &bus_cfg, SPI_DMA_CH_AUTO);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "Failed to initialize SPI bus (%s)", esp_err_to_name(err));
        return err;
    }
    s_owns_bus = true;

    err = mount_on(BOOT_SPI_HOST, 10000, false);  // Same clock as the runtime mount
    if (err != ESP_OK) {
        spi_bus_free(BOOT_SPI_HOST);
        s_owns_bus = false;
    }
    return err;
}

// The display bus is set up without MISO; route the SD card's data-out pin to
// SPI2's input through the GPIO matrix so the card can be read on that bus.
void sdcard::share_display_bus()
{
    gpio_reset_pin(BOARD_SPI_MISO);
    gpio_set_direction(BOARD_SPI_MISO, GPIO_MODE_INPUT);
    gpio_set_pull_mode(BOARD_SPI_MISO, GPIO_PULLUP_ONLY);
    esp_rom_gpio_connect_in_signal(BOARD_SPI_MISO, spi_periph_signal[DISPLAY_SPI_HOST].spiq_in, false);
}

esp_err_t sdcard::mount(bool writable)
{
    if (s_card) {
        return ESP_OK;
    }
    s_owns_bus = false;
    return mount_on(DISPLAY_SPI_HOST, 10000, writable);  // Conservative clock: MISO goes through the GPIO matrix
}

void sdcard::unmount()
{
    if (s_card) {
        esp_vfs_fat_sdcard_unmount(MOUNT_POINT, s_card);
        s_card = NULL;
        deselect_card();
    }
    if (s_owns_bus) {
        spi_bus_free(BOOT_SPI_HOST);
        s_owns_bus = false;
    }
}

int sdcard::load_ssh_keys(SSHTerminal* terminal)
{
    // Open the ssh_keys directory
    const char* keys_dir = "/sdcard/ssh_keys";
    ESP_LOGI(TAG, "Opening directory: %s", keys_dir);
    DIR* dir = opendir(keys_dir);
    
    if (!dir) {
        ESP_LOGW(TAG, "No %s directory", keys_dir);  // Reading never changes the card
        return 0;
    }

    ESP_LOGI(TAG, "Directory opened, scanning for .pem files...");

    // Read all .pem files from directory
    struct dirent* entry;
    int keys_loaded = 0;
    int files_found = 0;
    
    while ((entry = readdir(dir)) != NULL) {
        files_found++;
        ESP_LOGI(TAG, "Found file: %s", entry->d_name);
        
        // Skip . and .. entries
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        
        // Check if file has .pem or .PEM extension (case-insensitive)
        size_t name_len = strlen(entry->d_name);
        if (name_len < 5) {
            ESP_LOGD(TAG, "Skipping file (name too short): %s", entry->d_name);
            continue;
        }
        
        const char* ext = entry->d_name + name_len - 4;
        if (strcasecmp(ext, ".pem") != 0) {
            ESP_LOGI(TAG, "Skipping non-PEM file: %s", entry->d_name);
            continue;
        }
        
        ESP_LOGI(TAG, "Processing PEM file: %s", entry->d_name);
        
        // Build full file path
        char filepath[256];
        snprintf(filepath, sizeof(filepath), "%s/%s", keys_dir, entry->d_name);
        
        // Open and read the key file
        FILE* f = fopen(filepath, "rb");
        if (!f) {
            ESP_LOGE(TAG, "Failed to open key file: %s", filepath);
            continue;
        }
        
        // Get file size
        fseek(f, 0, SEEK_END);
        long file_size = ftell(f);
        fseek(f, 0, SEEK_SET);
        
        if (file_size <= 0 || file_size > 16384) {
            ESP_LOGW(TAG, "Invalid key file size: %s (%ld bytes)", entry->d_name, file_size);
            fclose(f);
            continue;
        }
        
        // Allocate buffer and read key data
        char* key_data = (char*)malloc(file_size + 1);
        if (!key_data) {
            ESP_LOGE(TAG, "Failed to allocate memory for key: %s", entry->d_name);
            fclose(f);
            continue;
        }
        
        size_t bytes_read = fread(key_data, 1, file_size, f);
        fclose(f);
        
        if (bytes_read != (size_t)file_size) {
            ESP_LOGE(TAG, "Failed to read complete key file: %s", entry->d_name);
            memset(key_data, 0, file_size);
            free(key_data);
            continue;
        }
        
        key_data[file_size] = '\0';  // Null terminate
        
        // Load key into terminal's memory
        terminal->load_key_from_memory(entry->d_name, key_data, file_size);
        memset(key_data, 0, file_size);  // Don't leave a copy of the private key in freed heap
        free(key_data);
        keys_loaded++;
    }
    
    closedir(dir);

    ESP_LOGI(TAG, "Files found: %d, keys loaded: %d", files_found, keys_loaded);
    return keys_loaded;
}
