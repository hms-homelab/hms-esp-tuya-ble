# The following lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

# Read version from version.txt (also used by ESP-IDF for esp_app_desc)
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/version.txt" PROJECT_VER)
string(STRIP "${PROJECT_VER}" PROJECT_VER)

# Build number from git commit count
execute_process(
    COMMAND git rev-list --count HEAD
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    OUTPUT_VARIABLE BUILD_NUMBER
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
)
if(NOT BUILD_NUMBER)
    set(BUILD_NUMBER "0")
endif()

# Short git hash
execute_process(
    COMMAND git rev-parse --short HEAD
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    OUTPUT_VARIABLE GIT_HASH
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
)
if(NOT GIT_HASH)
    set(GIT_HASH "unknown")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(tuya-ble-mqtt-bridge)

# Pass version info as compile definitions to all components
idf_build_set_property(COMPILE_DEFINITIONS
    "APP_VERSION=\"${PROJECT_VER}\""
    APPEND
)
idf_build_set_property(COMPILE_DEFINITIONS
    "BUILD_NUMBER=${BUILD_NUMBER}"
    APPEND
)
idf_build_set_property(COMPILE_DEFINITIONS
    "BUILD_VERSION=\"${PROJECT_VER}+${BUILD_NUMBER}.${GIT_HASH}\""
    APPEND
)
