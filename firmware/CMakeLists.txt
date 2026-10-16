cmake_minimum_required(VERSION 3.16)
if(CMAKE_SCRIPT_MODE_FILE)
    return()
endif()

# Board selection: idf.py menuconfig -> Agent Link Device -> Board Type (see boards/README.md).
# Target-wide sdkconfig defaults (PSRAM on, flash size, ...) live in sdkconfig.defaults /
# sdkconfig.defaults.<target> - native ESP-IDF, applied automatically by `idf.py set-target`.
if(DEFINED SDKCONFIG AND NOT "${SDKCONFIG}" STREQUAL "")
    get_filename_component(_sdkconfig "${SDKCONFIG}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_LIST_DIR}")
else()
    set(_sdkconfig "${CMAKE_CURRENT_LIST_DIR}/sdkconfig")
endif()
if(EXISTS "${_sdkconfig}")
    file(STRINGS "${_sdkconfig}" _sdkconfig_lines)

    set(_selected_board "")
    foreach(_line IN LISTS _sdkconfig_lines)
        if(_line MATCHES "^CONFIG_(BOARD_TYPE_[A-Z0-9_]+)=y$")
            set(_selected_board "${CMAKE_MATCH_1}")
        endif()
    endforeach()

    # Explicit per-board wants for every managed choice member, both ways - so switching boards
    # back and forth via menuconfig always converges to the right value, not just the first hop.
    set(_s3_boards
        BOARD_TYPE_ROROLEE_S3
        BOARD_TYPE_ESP32S3_ES8311_VOICE BOARD_TYPE_ESP32S3_ES8311_ASR BOARD_TYPE_ESP32S3_GC2145_CAMERA
        BOARD_TYPE_ESP32S3_WORK_BADGE BOARD_TYPE_ESP32S3_KORVO BOARD_TYPE_ESP32S3_KORVO_CLOUD
        BOARD_TYPE_ROROLEE_MUSE BOARD_TYPE_WOWNNY_MUSE BOARD_TYPE_WOWNNY_OLD_MUSE
        BOARD_TYPE_M5STACK_STOPWATCH)
    set(_ext_crys_boards BOARD_TYPE_ROROLEE_S3 BOARD_TYPE_ESP32S3_WORK_BADGE
        BOARD_TYPE_ROROLEE_MUSE BOARD_TYPE_WOWNNY_MUSE)
    # TLS records in PSRAM: boards that run a network link next to a display and audio.
    set(_tls_psram_boards BOARD_TYPE_ESP32S3_KORVO_CLOUD BOARD_TYPE_ROROLEE_MUSE BOARD_TYPE_WOWNNY_MUSE
        BOARD_TYPE_WOWNNY_OLD_MUSE)
    # Static RAM in PSRAM too: BLE, Wi-Fi and TLS up at once (Muse pairing) next to a display and
    # audio leave no internal RAM otherwise - NimBLE's pools and the .bss of Wi-Fi, lwIP and BT go
    # to PSRAM. One hardware log showed 0 KB internal left once BLE started without this.
    set(_bss_psram_boards BOARD_TYPE_ROROLEE_MUSE BOARD_TYPE_WOWNNY_MUSE BOARD_TYPE_WOWNNY_OLD_MUSE)
    # A 16 KB TCP send buffer where speech goes up as it is spoken (Muse voice notes, ~43 KB/s).
    # lwIP's default 5.7 KB is four segments in flight, too few for fast retransmit, so one lost
    # segment holds the upload until lwIP's retransmission timer (half a second or more). Meta's
    # own firmware uses 16 KB. lwIP's buffers are in PSRAM on these boards.
    set(_deep_tcp_boards BOARD_TYPE_ROROLEE_MUSE BOARD_TYPE_WOWNNY_MUSE BOARD_TYPE_WOWNNY_OLD_MUSE)
    if("CONFIG_AGENT_LINK_TRANSPORT_WIFI=y" IN_LIST _sdkconfig_lines)
        list(APPEND _bss_psram_boards BOARD_TYPE_ESP32S3_KORVO_CLOUD)
        list(APPEND _deep_tcp_boards BOARD_TYPE_ESP32S3_KORVO_CLOUD)
    endif()

    set(_board_forced_options "")
    if("${_selected_board}" IN_LIST _s3_boards)
        list(APPEND _board_forced_options "CONFIG_SPIRAM_MODE_OCT=y" "CONFIG_SPIRAM_SPEED_80M=y")
        if("${_selected_board}" IN_LIST _ext_crys_boards)
            list(APPEND _board_forced_options "CONFIG_RTC_CLK_SRC_EXT_CRYS=y")
        else()
            list(APPEND _board_forced_options "CONFIG_RTC_CLK_SRC_INT_RC=y")
        endif()
        if("${_selected_board}" IN_LIST _tls_psram_boards)
            list(APPEND _board_forced_options "CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC=y")
        else()
            list(APPEND _board_forced_options "CONFIG_MBEDTLS_INTERNAL_MEM_ALLOC=y")
        endif()
        # A plain bool, but forced both ways here for the same reason as the choices: a value one
        # board wants must not stay behind in sdkconfig when another board is selected.
        # .bss in PSRAM turns SPIRAM_IGNORE_NOTFOUND off, which with SPIRAM_TRY_ALLOCATE_WIFI_LWIP
        # leaves Wi-Fi only static TX buffers (1.6 KB of internal RAM each): 8 here, as on Meta's
        # own S3 Muse boards. The others get dynamic ones back, or the static choice would stick.
        # LVGL's heap (CONFIG_LV_MEM_SIZE_KILOBYTES) follows: in PSRAM on these boards, so it can
        # be 256 KB - TinyTTF's glyph cache lives in it - while the others keep 64 KB of internal RAM.
        if("${_selected_board}" IN_LIST _bss_psram_boards)
            list(APPEND _board_forced_options "CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY=y"
                                              "CONFIG_BT_NIMBLE_MEM_ALLOC_MODE_EXTERNAL=y"
                                              "CONFIG_ESP_WIFI_STATIC_TX_BUFFER=y"
                                              "CONFIG_ESP_WIFI_STATIC_TX_BUFFER_NUM=8"
                                              "CONFIG_LV_MEM_SIZE_KILOBYTES=256")
        else()
            list(APPEND _board_forced_options "# CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY is not set"
                                              "CONFIG_BT_NIMBLE_MEM_ALLOC_MODE_INTERNAL=y"
                                              "CONFIG_ESP_WIFI_DYNAMIC_TX_BUFFER=y"
                                              "CONFIG_LV_MEM_SIZE_KILOBYTES=64")
        endif()
        if("${_selected_board}" IN_LIST _deep_tcp_boards)
            list(APPEND _board_forced_options "CONFIG_LWIP_TCP_SND_BUF_DEFAULT=16384")
        else()
            list(APPEND _board_forced_options "CONFIG_LWIP_TCP_SND_BUF_DEFAULT=5760")
        endif()
        if(_selected_board STREQUAL "BOARD_TYPE_ESP32S3_KORVO_CLOUD" AND
           "CONFIG_AGENT_LINK_TRANSPORT_WIFI=y" IN_LIST _sdkconfig_lines)
            list(APPEND _board_forced_options "CONFIG_LWIP_TCP_WND_DEFAULT=32768"
                                              "CONFIG_LWIP_TCP_RECVMBOX_SIZE=26"
                                              "CONFIG_COMPILER_OPTIMIZATION_SIZE=y")
        endif()
        if(_selected_board STREQUAL "BOARD_TYPE_ESP32S3_KORVO_CLOUD")
            list(APPEND _board_forced_options "CONFIG_ESP_CONSOLE_SECONDARY_NONE=y"
                                              "# CONFIG_ESP_CONSOLE_SECONDARY_USB_SERIAL_JTAG is not set"
                                              "# CONFIG_USJ_ENABLE_USB_SERIAL_JTAG is not set"
                                              "# CONFIG_ESP_PHY_ENABLE_USB is not set")
        endif()
    elseif(_selected_board STREQUAL "BOARD_TYPE_FOLOTOY_AI_PASSPORT")
        # C3 has no PSRAM. Defaults do not override values in an existing sdkconfig.
        list(APPEND _board_forced_options "CONFIG_RTC_CLK_SRC_INT_RC=y")
        if("CONFIG_AGENT_LINK_TRANSPORT_BLE=y" IN_LIST _sdkconfig_lines)
            list(APPEND _board_forced_options "CONFIG_BT_NIMBLE_MSYS_1_BLOCK_COUNT=96"
                                              "CONFIG_BT_NIMBLE_MSYS_2_BLOCK_COUNT=96")
        endif()
    endif()

    set(_to_append "")
    foreach(_opt IN LISTS _board_forced_options)
        if(NOT "${_opt}" IN_LIST _sdkconfig_lines)
            list(APPEND _to_append "${_opt}")
        endif()
    endforeach()

    if(_to_append)
        list(JOIN _to_append "\n" _to_append_text)
        file(APPEND "${_sdkconfig}" "\n# board-forced options, patched by CMakeLists.txt (Kconfig select can't force choice members)\n${_to_append_text}\n")
        message(STATUS "agent_link: patched sdkconfig for ${_selected_board}: ${_to_append}")
    endif()
endif()

# Firmware version
set(PROJECT_VER "2.0.0")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(agent_link)
