# Sources shared by every board
set(SOURCES
    "deck_base.cpp"
    "ssh_terminal.cpp"
    "command_redact.cpp"
    "connection_profiles.cpp"
    "profile_menu.cpp"
    "wifi_menu.cpp"
    "vault_menu.cpp"
    "secret_vault.cpp"
    "known_hosts.cpp"
    "sd_card.cpp"
    "settings_nvs.cpp"
    "settings_backup.cpp"
    "storage_menu.cpp"
    "home_menu.cpp"
    "chat_app.cpp"
    "openai_client.cpp"
    "audio.cpp"
)

# List of include directories
set(INCLUDE_DIRS
    "."
    "include"
)

# Required components
set(REQUIRED_COMPONENTS
    lvgl
    nvs_flash
    esp_partition
    app_update
    fatfs
    sdmmc
    esp_wifi
    esp_netif
    esp_event
    esp_timer
    esp_http_client
    json
    esp_driver_i2s
    esp_driver_gpio
    esp_driver_i2c
    esp_adc
    mbedtls
)

# The build target picks the board (main/include/board.hpp):
#   esp32s3 - LilyGO T-Deck / T-Deck Plus (ESP32-C3 keyboard, trackball, ST7789 320x240)
#   esp32p4 - M5Stack Tab5 with the Tab5 Keyboard (MIPI-DSI 1280x720, WiFi on its ESP32-C6)
if(IDF_TARGET STREQUAL "esp32p4")
    list(APPEND SOURCES
        "board_tab5.cpp"
        "tab5_keyboard.cpp"
        "tab5_keymap.cpp"
        "battery_tab5.cpp"
        "audio_tab5.cpp"
    )
    # m5stack_tab5 (BSP) and esp_wifi_remote come from main/idf_component.yml
else()
    list(APPEND SOURCES
        "board_tdeck.cpp"
        "c3_keyboard.cpp"
        "battery_measurement.cpp"
        "audio_tdeck.cpp"
    )
    list(APPEND REQUIRED_COMPONENTS esp_lcd_touch_gt911)
endif()

idf_component_register(
    SRCS ${SOURCES}
    INCLUDE_DIRS ${INCLUDE_DIRS}
    REQUIRES ${REQUIRED_COMPONENTS}
)

# Add version number as compile definition
target_compile_definitions(${COMPONENT_LIB} PRIVATE SIMPLRSSH_VERSION="${PROJECT_VER}")
