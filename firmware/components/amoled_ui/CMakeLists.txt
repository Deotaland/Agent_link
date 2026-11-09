include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/profile_build.cmake")
if(NOT PROFILE_ui_component STREQUAL "amoled_ui")
    idf_component_register()
    return()
endif()
set(_requires board_support agent_link ui_lvgl)
if(NOT PROFILE_factory STREQUAL "")
    list(APPEND _requires agent_system)
endif()
idf_component_register(SRCS "amoled_ui.cc" "amoled_fonts.cc" "amoled_system_menu.cc" "amoled_display_transfer.cc"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "${FIRMWARE_ROOT}/boards/${PROFILE_board}" "${CMAKE_BINARY_DIR}/generated"
    REQUIRES ${_requires}
    PRIV_REQUIRES nvs_flash esp_timer)
if(AGENT_LINK_IMAGE_ROLE STREQUAL "factory")
    target_compile_options(${COMPONENT_LIB} PRIVATE -O2)
endif()
