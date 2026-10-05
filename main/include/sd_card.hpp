/*
 * SD Card
 * The T-Deck's SD card shares its SPI pins with the display. At boot (before
 * the display starts) it is mounted on its own SPI3 bus; afterwards it is
 * mounted as a second device on the display's SPI2 bus. The Tab5's card has its
 * own SDMMC slot. Mount only for the duration of an operation, while holding
 * the display lock.
 * Mounts are read-only unless writable is asked for; the card is never formatted.
 */

#ifndef SD_CARD_HPP
#define SD_CARD_HPP

#include "esp_err.h"

class SSHTerminal;

namespace sdcard
{
    extern const char* const MOUNT_POINT;   // "/sdcard"

    esp_err_t mount_at_boot();               // Before bsp_display_start(); read-only
    void share_display_bus();                // Once, right after bsp_display_start()
    esp_err_t mount(bool writable = false);  // At runtime, on the display bus
    void unmount();

    // Loads *.pem files from /sdcard/ssh_keys into the terminal; card must be mounted
    int load_ssh_keys(SSHTerminal* terminal);
}

#endif
