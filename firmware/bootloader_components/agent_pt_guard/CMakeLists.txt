# Read the parent's resolved plan in both IDF dependency expansion and normal
# configuration. Ninja invokes the bootloader in a fresh process without the
# environment set by the parent's CMake configure step.
idf_build_get_property(_boot_build BUILD_DIR)
get_filename_component(_plan_path "${_boot_build}/../flash-plan.json" ABSOLUTE)
file(READ "${_plan_path}" _plan)
string(JSON _dynamic ERROR_VARIABLE _dynamic_error GET "${_plan}" dynamic_slots)
if(_dynamic_error)
    idf_component_register()
    return()
endif()
string(JSON _profile GET "${_plan}" profile)
get_filename_component(_firmware "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
execute_process(COMMAND "${PYTHON}" "${_firmware}/tools/dynamic_layout.py"
    --profile "${_profile}" --header "${CMAKE_CURRENT_BINARY_DIR}/dynamic_layout_config.h"
    RESULT_VARIABLE _result ERROR_VARIABLE _error)
if(NOT _result EQUAL 0)
    message(FATAL_ERROR "${_error}")
endif()
idf_component_register(SRCS "guard.c"
    INCLUDE_DIRS "${CMAKE_CURRENT_BINARY_DIR}" "${_firmware}/components/agent_system/include"
    PRIV_INCLUDE_DIRS "${IDF_PATH}/components/bootloader_support/private_include"
    REQUIRES bootloader_support esp_rom spi_flash)
target_link_libraries(${COMPONENT_LIB} INTERFACE "-u bootloader_after_init" "-u agent_pt_guard_marker")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${_plan_path}"
    "${_firmware}/partitions/layouts.json" "${_firmware}/partitions/sdgoods-v4.csv"
    "${_firmware}/partitions/sdgoods-v5.csv"
    "${_firmware}/tools/dynamic_layout.py")
