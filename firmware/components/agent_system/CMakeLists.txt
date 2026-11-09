include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/profile_build.cmake")
if(PROFILE_factory STREQUAL "")
    idf_component_register()
    return()
endif()
if(IDF_VERSION_MAJOR GREATER_EQUAL 6)
    set(_json cjson)
else()
    set(_json json)
endif()
idf_component_register(SRCS "agent_system.cc" "cover_cache.cc" INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "../agent_link/src/ota"
    REQUIRES esp_common esp_partition esp_app_format
    PRIV_REQUIRES app_update bootloader_support nvs_flash esp_http_client esp_http_server
                  esp_netif esp_timer heap mbedtls ${_json})
string(JSON _model GET "${SYSTEM_MANIFEST}" model)
string(JSON _layout GET "${SYSTEM_MANIFEST}" layout)
string(JSON _slots LENGTH "${BUILD_PROFILE}" layout_info app_slots)
string(JSON _reserve GET "${BUILD_PROFILE}" layout_info cover_reserve)
string(JSON _slot_size GET "${BUILD_PROFILE}" layout_info partitions ota_0 size)
math(EXPR _app_limit "${_slot_size} - ${_reserve}")
target_compile_definitions(${COMPONENT_LIB} PUBLIC AGENT_SYSTEM_ENABLED=1
    AGENT_SYSTEM_BOARD_MODEL="${_model}" AGENT_SYSTEM_LAYOUT=${_layout}
    AGENT_SYSTEM_SLOT_COUNT=${_slots} AGENT_SYSTEM_COVER_RESERVE=${_reserve}
    AGENT_SYSTEM_APP_MAX_BYTES=${_app_limit})
target_compile_definitions(${COMPONENT_LIB} PRIVATE
    AGENT_SYSTEM_APP_ID="${IMAGE_app_id}" AGENT_SYSTEM_APP_NAME="${IMAGE_name}")
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-missing-field-initializers)
