# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

# Firmware version. CI sets FIRMWARE_VERSION to the release number it computes
# (latest vX.Y.Z tag + patch bump, see .github/scripts/plan_release.py).
# The value below is used for local builds and as the minimum release version:
# raise it to jump to a new minor/major version.
if(DEFINED ENV{FIRMWARE_VERSION})
    set(PROJECT_VER "$ENV{FIRMWARE_VERSION}")
else()
    set(PROJECT_VER "1.4.0")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# One source tree, two boards: the target picks the board (main/CMakeLists.txt) and its
# component lock file. esp32s3 = LilyGO T-Deck (dependencies.lock, sdkconfig),
# esp32p4 = M5Stack Tab5 (dependencies.lock.esp32p4, sdkconfig.tab5, sdkconfig.defaults.esp32p4):
#   idf.py -B build-tab5 -D IDF_TARGET=esp32p4 -D SDKCONFIG=sdkconfig.tab5 build
if(IDF_TARGET STREQUAL "esp32p4")
    idf_build_set_property(DEPENDENCIES_LOCK "${CMAKE_CURRENT_LIST_DIR}/dependencies.lock.esp32p4")
endif()

project(SimplrSSH)

# Image for launchers such as bmorcelli/Launcher, which install apps into their own partition
# table: the same bootloader and app as the merged release image, with partitions_launcher.csv
# (adds the "simplrssh" settings partition). `idf.py launcher-bin` after `idf.py build`
# writes <build>/SimplrSSH-v<version>-launcher.bin (Tab5: SimplrSSH-Tab5-v<version>-launcher.bin).
idf_build_get_property(python PYTHON)
idf_build_get_property(idf_path IDF_PATH)
idf_build_get_property(build_dir BUILD_DIR)
set(LAUNCHER_TABLE "${build_dir}/partition_table/partition-table-launcher.bin")
if(IDF_TARGET STREQUAL "esp32p4")
    set(IMAGE_PREFIX "SimplrSSH-Tab5")
else()
    set(IMAGE_PREFIX "SimplrSSH")
endif()
set(LAUNCHER_APP_OFFSET 0x50000)   # the app's offset in partitions_launcher.csv
add_custom_target(launcher-bin
    COMMAND ${python} ${idf_path}/components/partition_table/gen_esp32part.py -q
            --flash-size ${CONFIG_ESPTOOLPY_FLASHSIZE} --offset ${CONFIG_PARTITION_TABLE_OFFSET}
            ${CMAKE_SOURCE_DIR}/partitions_launcher.csv ${LAUNCHER_TABLE}
    COMMAND ${python} -m esptool --chip ${IDF_TARGET} merge_bin
            -o ${build_dir}/${IMAGE_PREFIX}-v${PROJECT_VER}-launcher.bin
            --flash_mode ${CONFIG_ESPTOOLPY_FLASHMODE} --flash_freq ${CONFIG_ESPTOOLPY_FLASHFREQ}
            --flash_size ${CONFIG_ESPTOOLPY_FLASHSIZE}
            ${CONFIG_BOOTLOADER_OFFSET_IN_FLASH} ${build_dir}/bootloader/bootloader.bin
            ${CONFIG_PARTITION_TABLE_OFFSET} ${LAUNCHER_TABLE}
            ${LAUNCHER_APP_OFFSET} ${build_dir}/${PROJECT_NAME}.bin
    VERBATIM)
add_dependencies(launcher-bin app bootloader)
