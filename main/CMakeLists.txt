# List of source files
set(SOURCES
    "deck_base.cpp"
    "battery_measurement.cpp"
    "c3_keyboard.cpp"
    "ssh_terminal.cpp"
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
    esp_lcd_touch_gt911
    fatfs
    sdmmc
    esp_wifi
    esp_netif
    esp_event
    esp_timer
    esp_http_client
    json
    esp_driver_i2s
    mbedtls
)

idf_component_register(
    SRCS ${SOURCES}
    INCLUDE_DIRS ${INCLUDE_DIRS}
    REQUIRES ${REQUIRED_COMPONENTS}
)

# Add version number as compile definition
target_compile_definitions(${COMPONENT_LIB} PRIVATE SIMPLRSSH_VERSION="${PROJECT_VER}")
