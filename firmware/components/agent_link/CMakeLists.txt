idf_component_register(
    SRCS
        "src/agent_link.cpp"
        "src/transport_ble.cpp"
        "src/transport_wifi.cpp"
        "src/wifi_provision.cpp"
        "src/ota_service.cpp"
        "src/audio_downlink.cpp"
        "src/protocol.cpp"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "src"
    REQUIRES esp_common
    PRIV_REQUIRES log bt nvs_flash freertos
                  esp_wifi esp_netif esp_event esp_http_server esp_timer lwip
                  app_update esp_partition esp_app_format mbedtls
    EMBED_TXTFILES "portal/portal.html" "portal/connecting.html"
)

# Designated-initializer structs here deliberately set only the fields they care about and leave the
# rest zero-initialised. Without this the build prints a screenful of -Wmissing-field-initializers
# for every one of them, which drowns out warnings that matter. main/CMakeLists.txt does the same.
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-missing-field-initializers)
