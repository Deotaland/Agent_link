set(SOURCES "app_main.cpp")
set(INCLUDE_DIRS "." "${CMAKE_CURRENT_SOURCE_DIR}/../boards/common")

file(GLOB COMMON_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/../boards/common/*.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../boards/common/*.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../boards/common/*.c)
list(APPEND SOURCES ${COMMON_SOURCES})

if(CONFIG_BOARD_TYPE_ROROLEE_S3)
    set(BOARD_DIR "rorolee-s3")
elseif(CONFIG_BOARD_TYPE_ESP32P4_WAVESHARE)
    set(BOARD_DIR "esp32p4-waveshare")
elseif(CONFIG_BOARD_TYPE_ESP32S3_TEM_MONITOR)
    set(BOARD_DIR "tem-monitor")
elseif(CONFIG_BOARD_TYPE_ESP32S3_ES8311_VOICE)
    set(BOARD_DIR "es8311-voice")
elseif(CONFIG_BOARD_TYPE_ESP32S3_ES8311_ASR)
    set(BOARD_DIR "es8311-asr")
elseif(CONFIG_BOARD_TYPE_ESP32S3_GC2145_CAMERA)
    set(BOARD_DIR "gc2145-camera")
elseif(CONFIG_BOARD_TYPE_ESP32S3_KORVO)
    set(BOARD_DIR "korvo")
elseif(CONFIG_BOARD_TYPE_ESP32S3_KORVO_CLOUD)
    set(BOARD_DIR "korvo-cloud")
elseif(CONFIG_BOARD_TYPE_ESP32S3_WORK_BADGE)
    set(BOARD_DIR "work-badge")
elseif(CONFIG_BOARD_TYPE_ROROLEE_MUSE)
    set(BOARD_DIR "rorolee-muse")
# Add a new board: add a line elseif(CONFIG_BOARD_TYPE_XXX) set(BOARD_DIR "your-board")
endif()

# rorolee-muse: an optional avatar renderer, never committed (boards/rorolee-muse/avatar holds it,
# and its .gitignore keeps it out of git). Present, it is built and the UI draws it.
set(ROROLEE_MUSE_AVATAR "${CMAKE_CURRENT_SOURCE_DIR}/../boards/rorolee-muse/avatar/muse_pixel.c")
if(CONFIG_BOARD_TYPE_ROROLEE_MUSE AND EXISTS "${ROROLEE_MUSE_AVATAR}")
    list(APPEND SOURCES "${ROROLEE_MUSE_AVATAR}")
    set(ROROLEE_MUSE_HAS_AVATAR 1)
endif()

if(BOARD_DIR)
    file(GLOB BOARD_SOURCES
        ${CMAKE_CURRENT_SOURCE_DIR}/../boards/${BOARD_DIR}/*.cc
        ${CMAKE_CURRENT_SOURCE_DIR}/../boards/${BOARD_DIR}/*.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../boards/${BOARD_DIR}/*.c
    )
    list(APPEND SOURCES ${BOARD_SOURCES})
elseif(NOT CMAKE_BUILD_EARLY_EXPANSION)
    message(FATAL_ERROR "No board selected. Run: idf.py menuconfig -> Agent Link Device -> Board Type")
endif()

# REQUIRES CANNOT BE CONDITIONAL ON CONFIG_*.
#
# ESP-IDF collects REQUIRES in a first "early expansion" pass that runs this file in a child process
# in CMake script mode, where sdkconfig has not been loaded — so every CONFIG_* reads as empty and an
# if(CONFIG_BOARD_TYPE_...) around a REQUIRES silently contributes nothing. (SRCS above is fine: that
# is re-evaluated in the normal pass, which is why the board-select chain works.)
#
# This is survivable for most of the per-board components only by luck:
#   - esp_lcd_co5300, esp32-camera and lvgl are in main/idf_component.yml, and the component manager
#     injects managed dependencies into main's PRIV_REQUIRES on its own. Their target rules there are
#     also what keeps a P4-only or S3-only component out of the wrong build — something an
#     unconditional REQUIRES here could NOT do, since a missing component is a hard error.
#   - esp_driver_rmt (gc2145-camera's WS2812) arrives through the common `driver` umbrella.
# Anything outside those two categories has to be listed unconditionally below. fatfs is the first
# one that was not covered, it publicly requires sdmmc/esp_driver_sdmmc,so the TF card needs no separate entry.
#
# Adding a board that needs a NEW component: put it in idf_component.yml if it is a managed
# dependency (with a `rules:` target gate when it is target-specific), otherwise add it to the list
# below. Do not add an if(CONFIG_...) block here expecting it to work.

idf_component_register(
    SRCS ${SOURCES}
    INCLUDE_DIRS ${INCLUDE_DIRS}
    REQUIRES agent_link esp_driver_gpio esp_driver_spi esp_lcd esp_lcd_sh8501 esp_lcd_sh8501_lk
             esp_codec_dev esp_driver_i2c esp_driver_i2s nvs_flash esp_partition
             fatfs)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-missing-field-initializers)

# rorolee-muse: LVGL's heap is a static 64 KB array (LV_MEM_SIZE) that would otherwise sit in
# internal RAM, which BLE, Wi-Fi and TLS need at once while it pairs. The repo-root CMakeLists turns
# on SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY for this board, and EXT_RAM_BSS_ATTR then puts the array
# in PSRAM; LVGL leaves LV_ATTRIBUTE_LARGE_RAM_ARRAY to be defined from outside. (Target properties,
# not REQUIRES, so a CONFIG_ test is fine here.)
if(ROROLEE_MUSE_HAS_AVATAR)
    message(STATUS "rorolee-muse: avatar renderer found, the UI will draw it")
    target_compile_definitions(${COMPONENT_LIB} PRIVATE VOICE_UI_AVATAR=1)
endif()
if(CONFIG_BOARD_TYPE_ROROLEE_MUSE)
    idf_component_get_property(lvgl_lib lvgl__lvgl COMPONENT_LIB)
    target_compile_options(${lvgl_lib} PRIVATE -include esp_attr.h)
    target_compile_definitions(${lvgl_lib} PRIVATE LV_ATTRIBUTE_LARGE_RAM_ARRAY=EXT_RAM_BSS_ATTR)
endif()
