get_filename_component(_firmware "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${_firmware}/tools/dynamic_layout.py"
    "${_firmware}/partitions/layouts.json" "${_firmware}/partitions/sdgoods-v5.csv")
execute_process(COMMAND "${PYTHON}" "${_firmware}/tools/dynamic_layout.py"
    --profile sdgoods-wifi --header "${CMAKE_CURRENT_BINARY_DIR}/dynamic_layout_config.h"
    RESULT_VARIABLE _result)
if(NOT _result EQUAL 0)
    message(FATAL_ERROR "Cannot generate recovery layout")
endif()
idf_component_register(SRCS "recovery.c"
    "${_firmware}/boards/sdgoods-esp32s3/sdgoods_power.c"
    INCLUDE_DIRS "${CMAKE_CURRENT_BINARY_DIR}" "${_firmware}/boards/sdgoods-esp32s3"
    "${_firmware}/components/agent_system/include"
    REQUIRES app_update esp_driver_gpio spi_flash esp_rom bootloader_support esp_timer)
# Keep the routing image identity even though app_main does not read it.
target_link_libraries(${COMPONENT_LIB} INTERFACE "-u agent_recovery_identity")
