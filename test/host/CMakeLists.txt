# Host unit tests for the firmware's hardware-independent code (CI "Tests" workflow).
# The firmware sources in main/ are compiled unchanged for Linux against small stand-ins
# for the ESP-IDF headers they use (idf_stubs/, fake_idf.cpp: in-memory NVS, partition
# table, captured log). Crypto is the host's mbed TLS (libmbedtls-dev).
#
#   cmake -S test/host -B build-host && cmake --build build-host && ctest --test-dir build-host
#
# Two suites, run by separate CI jobs: "unit" and "persistence" (settings partition choice,
# backup/restore, records written by older firmware).
cmake_minimum_required(VERSION 3.16)
project(simplrssh_host_tests CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(MAIN_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../main)

option(STRICT "Treat warnings as errors" ON)
add_compile_options(-Wall -Wextra -Wformat=2 -fno-common)
if(STRICT)
    add_compile_options(-Werror)
endif()
option(SANITIZE "Build with AddressSanitizer and UBSan" ON)
if(SANITIZE)
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all)
    add_link_options(-fsanitize=address,undefined)
endif()

find_library(MBEDCRYPTO mbedcrypto REQUIRED)

# Firmware sources under test, compiled exactly as they are in main/
add_library(firmware STATIC
    ${MAIN_DIR}/command_redact.cpp
    ${MAIN_DIR}/connection_profiles.cpp
    ${MAIN_DIR}/secret_vault.cpp
    ${MAIN_DIR}/settings_backup.cpp
    ${MAIN_DIR}/settings_nvs.cpp
    ${MAIN_DIR}/tab5_keymap.cpp
    ${MAIN_DIR}/vterm.cpp
    fake_idf.cpp
)
# The stand-ins come first so they shadow nothing else; main/include is the firmware's own
target_include_directories(firmware PUBLIC idf_stubs ${MAIN_DIR}/include ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(firmware PUBLIC ${MBEDCRYPTO})

add_executable(unit_tests test_main.cpp
    tests/test_command_redact.cpp
    tests/test_connection_profiles.cpp
    tests/test_secret_vault.cpp
    tests/test_tab5_keymap.cpp
    tests/test_vterm.cpp
)
target_link_libraries(unit_tests firmware)

add_executable(persistence_tests test_main.cpp
    tests/test_settings_nvs.cpp
    tests/test_settings_backup.cpp
    tests/test_profile_migration.cpp
)
target_link_libraries(persistence_tests firmware)

enable_testing()
add_test(NAME unit COMMAND unit_tests)
set_tests_properties(unit PROPERTIES LABELS unit)
add_test(NAME persistence COMMAND persistence_tests)
set_tests_properties(persistence PROPERTIES LABELS persistence)
