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
