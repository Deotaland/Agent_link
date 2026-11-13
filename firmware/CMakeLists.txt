cmake_minimum_required(VERSION 3.22)
if(CMAKE_SCRIPT_MODE_FILE)
    return()
endif()

include(cmake/firmware_profile.cmake)
set(COMPONENTS main)
set(PROJECT_VER "2.0.0")
if(AGENT_LINK_PROFILE STREQUAL "sdgoods-wifi" AND AGENT_LINK_IMAGE_ROLE STREQUAL "factory")
    file(STRINGS "${CMAKE_CURRENT_LIST_DIR}/factory/sdgoods-esp32s3/launcher_version.h" _launcher_version
        REGEX "^#define AGENT_LAUNCHER_VERSION ")
    string(REGEX REPLACE ".*\"([^\"]+)\".*" "\\1" PROJECT_VER "${_launcher_version}")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_LIST_DIR}/factory/sdgoods-esp32s3/launcher_version.h")
endif()
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
idf_build_set_property(DEPENDENCIES_LOCK "${DEPENDENCIES_LOCK}")
project(agent_link)
# Vendor applications recognize their host through the standard IDF descriptor.
# Override only that source's metadata; keep output filenames and OTA model stable.
if(AGENT_LINK_PROFILE STREQUAL "sdgoods-wifi" AND AGENT_LINK_IMAGE_ROLE STREQUAL "factory")
    set_source_files_properties("$ENV{IDF_PATH}/components/esp_app_format/esp_app_desc.c"
        TARGET_DIRECTORY __idf_esp_app_format PROPERTIES COMPILE_DEFINITIONS
        "PROJECT_VER=\"${PROJECT_VER}\";PROJECT_NAME=\"SDGOODS_LAUNCHER\"")
endif()
agent_link_validate_profile()
include(cmake/profile_flash.cmake)
