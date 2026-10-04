# muse_gadget: Meta's Muse Home Link (link/, Apache-2.0, vendored from muse-gadget-sdk/esp32/main)
# plus the port layer (port/) that runs it as agent_link's Muse transport. Every change made to
# link/ is marked MUSE_GADGET_HOSTED or explained where it was made.
#
# agent_link requires this component on every build, and a REQUIRES cannot depend on Kconfig, so
# the requirement lists below are unconditional and only the sources follow the transport choice:
# on BLE and WiFi this is an empty component and nothing of it is linked.

if(CONFIG_AGENT_LINK_TRANSPORT_MUSE)
    set(srcs
        "link/app.c"
        "link/ble_server.c"
        "link/bug_report.c"
        "link/button.c"
        "link/config_store.c"
        "link/customer_product_info.c"
        "link/diagnostic_log.c"
        "link/factory_test.c"
        "link/identity.c"
        "link/image_fetch.c"
        "link/link_pairing.c"
        "link/net_discovery.c"
        "link/noise_control.cpp"
        "link/noise_tunnel.cpp"
        "link/ota.c"
        "link/pairing_signer_policy.c"
        "link/pairing_transcript.c"
        "link/stack_monitor.c"
        "link/tunnel_netif.c"
        "link/vm_api.c"
        "link/wifi_known.c"
        "link/wifi_mgr.c"
        "port/muse_gadget.c"
        "port/led_status_gadget.c"
        "port/muse_note.c")
else()
    set(srcs)
endif()

idf_component_register(
    SRCS ${srcs}
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "link" "port"
    REQUIRES esp_common
    PRIV_REQUIRES nvs_flash esp_wifi esp_event esp_netif esp_http_client esp_https_ota
                  app_update esp_app_format esp-tls esp_timer esp_partition esp_driver_gpio
                  bt lwip mbedtls efuse json noise_core
)

if(srcs)
    # Upstream builds these at -O2 with its own warning set; keep the code as written rather than
    # restyle Meta's sources to this project's warnings.
    target_compile_options(${COMPONENT_LIB} PRIVATE
        -Wno-missing-field-initializers -Wno-format-truncation -Wno-unused-function
        -Wno-unused-variable -Wno-unused-but-set-variable)
    # Upstream switches between its own boards with CONFIG_MUSE_ENABLED; this marks the third
    # case: Home Link hosted by agent_link, which supplies the screen, buttons and voice turn.
    target_compile_definitions(${COMPONENT_LIB} PRIVATE MUSE_GADGET_HOSTED=1)

    # The check Meta's SDK makes at configure time: a token from gadgets.muse.ai is mgst_ plus 43
    # canonical base64url characters.
    if("${CONFIG_GADGET_SDK_TOKEN}" STREQUAL "")
        message(WARNING "Muse transport without an SDK token: set Component config -> Muse gadget -> "
                        "SDK token to the mgst_ token from gadgets.muse.ai. Gadgets without one will "
                        "stop pairing once Muse requires tokens.")
    else()
        string(LENGTH "${CONFIG_GADGET_SDK_TOKEN}" _token_len)
        if(NOT _token_len EQUAL 48 OR
           NOT CONFIG_GADGET_SDK_TOKEN MATCHES "^mgst_[A-Za-z0-9_-]*[AEIMQUYcgkosw048]$")
            message(FATAL_ERROR "CONFIG_GADGET_SDK_TOKEN is not a valid SDK token. Copy it again from "
                                "gadgets.muse.ai.")
        endif()
    endif()
endif()
