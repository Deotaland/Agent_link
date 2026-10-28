# The firmware passes its fixed backend before IDF expands component requirements.
# External SDK consumers without a profile retain the legacy multi-backend build.
set(_backend "$ENV{AGENT_LINK_BUILD_BACKEND}")
if(_backend STREQUAL "NONE")
    idf_component_register()
    return()
endif()
if(NOT _backend STREQUAL "" AND NOT _backend MATCHES "^(BLE|WIFI|MUSE)$")
    message(FATAL_ERROR "AGENT_LINK_BUILD_BACKEND must be BLE, WIFI or MUSE")
endif()
set(_sources src/core/agent_link.cpp src/core/protocol.cpp src/core/device_identity.cc
             src/audio/audio_downlink.cpp src/ota/ota_service.cpp src/transports/disabled.cpp)
set(_requires log nvs_flash freertos esp_timer app_update esp_partition esp_app_format mbedtls)
set(_assets "")
set(_desktop_ble 0)
if("$ENV{AGENT_LINK_BUILD_PROFILE}" STREQUAL "paper-s31-wifi")
    set(_desktop_ble 1)
    list(APPEND _requires bt)
endif()
set(_ble 0)
set(_wifi 0)
set(_muse 0)
set(_json json)
if(IDF_VERSION_MAJOR GREATER_EQUAL 6)
    set(_json cjson)
endif()
if(_backend STREQUAL "BLE" OR _backend STREQUAL "")
    set(_ble 1)
    list(APPEND _sources src/transports/ble/transport_ble.cpp)
    list(APPEND _requires bt)
endif()
if(_backend STREQUAL "WIFI" OR _backend STREQUAL "")
    list(APPEND _requires esp_wifi esp_netif esp_event esp_http_server lwip esp_http_client
                          ${_json} mqtt tcp_transport)
    if(NOT IDF_TARGET STREQUAL "esp32s31")
        list(APPEND _requires esp_audio_codec)
    endif()
    if(NOT (CONFIG_IDF_TARGET_ESP32C3 AND CONFIG_AGENT_LINK_TRANSPORT_BLE))
        set(_wifi 1)
        list(APPEND _sources src/transports/wifi/transport_wifi.cpp src/transports/wifi/wifi_provision.cpp
            src/transports/wifi/wifi_ble.cpp
            src/transports/wifi/cloud/cloud_credential.cc src/transports/wifi/cloud/cloud_api.cc
            src/transports/wifi/cloud/cloud_session.cc src/transports/wifi/cloud/cloud_mqtt.cc
            src/transports/wifi/cloud/cloud_realtime.cc src/transports/wifi/cloud/cloud_platform.cc
            src/transports/wifi/cloud/cloud_chat.cc
            src/transports/wifi/cloud/realtime_codec_memory.cc
            src/transports/wifi/cloud/cloud_json.cc src/transports/wifi/cloud/cloud_clock.cc)
        set(_assets portal/portal.html portal/connecting.html)
    endif()
endif()
if(_backend STREQUAL "MUSE" OR _backend STREQUAL "")
    set(_muse 1)
    list(APPEND _sources src/transports/muse/transport_muse.cpp)
    list(APPEND _requires muse_gadget esp_wifi ${_json})
endif()
idf_component_register(SRCS ${_sources} INCLUDE_DIRS include
    PRIV_INCLUDE_DIRS src/core src/audio src/ota src/transports/wifi src/transports/wifi/cloud
    REQUIRES esp_common PRIV_REQUIRES ${_requires} EMBED_TXTFILES ${_assets})
target_compile_definitions(${COMPONENT_LIB} PRIVATE
    AGENT_LINK_BUILD_BLE=${_ble} AGENT_LINK_BUILD_WIFI=${_wifi} AGENT_LINK_BUILD_MUSE=${_muse}
    AGENT_LINK_DESKTOP_BLE=${_desktop_ble})
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-missing-field-initializers)
