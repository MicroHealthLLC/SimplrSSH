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
    set(PROJECT_VER "1.3.0")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(PocketSSH)
